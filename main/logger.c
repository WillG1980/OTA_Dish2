/*
 * Logging verbosity (ESP-IDF):
 * You can adjust log levels at runtime using esp_log_level_set():
 *
 *   // Set global default level:
 *   esp_log_level_set("*", ESP_LOG_INFO);
 *
 *   // Per-tag override (this file’s tag is "TCP_LOGGER"):
 *   esp_log_level_set("TCP_LOGGER", ESP_LOG_DEBUG);
 *
 * Valid levels: ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN,
 *               ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE.
 *
 * Place these calls early (e.g., in app_main()) before heavy logging starts.
 * When TCP logging is disconnected, messages still echo to the serial console.
 */

#include "logger.h"
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdio.h>    // <-- added
#include <stdlib.h>   // <-- added
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "dishwasher_programs.h"

#define TAG "TCP_LOGGER"

// ---- Internal state ----
static char *log_buffer = NULL;
static size_t buffer_size = 0;
static size_t buffer_pos = 0;
static SemaphoreHandle_t buffer_mutex = NULL;
static int sock_fd = -1;
static TaskHandle_t sender_task_handle = NULL;
static vprintf_like_t s_saved_vprintf = NULL;
static bool s_hook_installed = false;

static volatile bool s_net_ready = false; // set by IP/Wi-Fi events

static esp_event_handler_instance_t s_ip_got_inst = NULL;
static esp_event_handler_instance_t s_ip_lost_inst = NULL;
static esp_event_handler_instance_t s_wifi_disc_inst = NULL;

// Connection target (copied in init; avoids dangling pointers)
static char s_host[64] = {0};
static uint16_t s_port = 0;

// Dedicated send buffer to avoid race with producer while sending
static char *send_buffer = NULL;
static size_t send_buffer_size = 0;

// ---- Helpers (NO LOGGING HERE to avoid recursion) ----
static void buffer_add(const char *data, size_t len) {
    if (!log_buffer || len == 0) return;
    xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    size_t space = buffer_size - buffer_pos;
    size_t to_copy = (len < space) ? len : space;
    if (to_copy > 0) {
        memcpy(log_buffer + buffer_pos, data, to_copy);
        buffer_pos += to_copy;
    }
    xSemaphoreGive(buffer_mutex);
}

static void try_connect_logger(void) {
    _LOG_D("Start of function");
    if (!s_net_ready || sock_fd >= 0 || s_host[0] == 0 || s_port == 0) { _LOG_D("Exiting function"); return; }

    _LOG_D("Preparing to open socket");

    // Validate IPv4 dotted-quad; skip if invalid
    in_addr_t addr = inet_addr(s_host);
    if (addr == INADDR_NONE) {
        _LOG_W("inet_addr failed for host '%s'", s_host);
        _LOG_D("Exiting function"); return;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(s_port);
    sa.sin_addr.s_addr = addr;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        _LOG_W("socket() failed, errno=%d", errno);
        _LOG_D("Exiting function"); return;
    }

    // Non-blocking connect with 1s timeout
    int flags = lwip_fcntl(fd, F_GETFL, 0);
    lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 }; // 1s
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            _LOG_W("connect timeout/error (select rc=%d, errno=%d)", rc, errno);
            close(fd);
            _LOG_D("Exiting function"); return;
        }
        // Check SO_ERROR
        int soerr = 0; socklen_t slen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
            _LOG_W("connect failed (SO_ERROR=%d)", soerr);
            close(fd);
            _LOG_D("Exiting function"); return;
        }
    } else if (rc < 0) {
        _LOG_W("connect() failed immediately, errno=%d", errno);
        close(fd);
        _LOG_D("Exiting function"); return;
    }

    // Back to blocking, and set send/recv timeouts (500 ms)
    lwip_fcntl(fd, F_SETFL, flags);
    struct timeval to = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    sock_fd = fd;
    _LOG_I("TCP logger connected to %s:%u", s_host, (unsigned)s_port);
    _LOG_D("Exiting function");
}

// ---- vprintf hook ----
static int tcp_logger_vprintf(const char *fmt, va_list args) {
    // IMPORTANT: No _LOG_* calls here (would recurse).
    if (!fmt) return 0;

    int ret_uart = 0;

    // 1) Figure out required size using a copy of args
    va_list ap1;
    va_copy(ap1, args);
    int needed = vsnprintf(NULL, 0, fmt, ap1);
    va_end(ap1);
    if (needed < 0) {
        // If sizing failed, try forwarding to saved vprintf anyway
        if (s_saved_vprintf) {
            va_list apf;
            va_copy(apf, args);
            ret_uart = s_saved_vprintf(fmt, apf);
            va_end(apf);
        }
        return ret_uart;
    }

    // 2) Allocate and format the message
    char *msg = (char *)malloc((size_t)needed + 1);
    if (!msg) {
        // Allocation failed; just forward to UART
        if (s_saved_vprintf) {
            va_list apf;
            va_copy(apf, args);
            ret_uart = s_saved_vprintf(fmt, apf);
            va_end(apf);
        }
        return ret_uart;
    }

    va_list ap2;
    va_copy(ap2, args);
    vsnprintf(msg, (size_t)needed + 1, fmt, ap2);
    va_end(ap2);

    // 3) Buffer to TCP if connected
    if (sock_fd >= 0) {
        buffer_add(msg, (size_t)needed);
    }

    // 4) Always forward to the original UART logger if available
    if (s_saved_vprintf) {
        va_list ap3;
        va_copy(ap3, args);
        ret_uart = s_saved_vprintf(fmt, ap3);
        va_end(ap3);
    }

    free(msg);
    // Return number of characters that printf would have produced
    return (ret_uart > 0) ? ret_uart : needed;
}

// ---- Event handling ----
static void net_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    _LOG_D("Start of function");
    (void)arg; (void)event_data;
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            _LOG_I("IP event: GOT_IP");
            s_net_ready = true;
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            _LOG_W("IP event: LOST_IP");
            s_net_ready = false;
            if (sock_fd >= 0) { close(sock_fd); sock_fd = -1; }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        _LOG_W("Wi-Fi event: DISCONNECTED");
        s_net_ready = false;
        if (sock_fd >= 0) { close(sock_fd); sock_fd = -1; }
    }
    _LOG_D("Exiting function");
}

// ---- Sender / reconnect task ----
static void sender_task(void *arg) {
    _LOG_D("Start of function");
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (sock_fd < 0 && s_net_ready) {
            _LOG_D("Attempting TCP connect...");
            try_connect_logger();
        }
        if (sock_fd >= 0) {
            // Copy out into send_buffer under mutex to avoid race with producer
            size_t n = 0;
            xSemaphoreTake(buffer_mutex, portMAX_DELAY);
            n = buffer_pos;
            if (n > 0 && send_buffer && n <= send_buffer_size) {
                memcpy(send_buffer, log_buffer, n);
            }
            buffer_pos = 0;
            xSemaphoreGive(buffer_mutex);

            if (n > 0) {
                const char *src = (send_buffer && n <= send_buffer_size) ? send_buffer : log_buffer;
                ssize_t sent = send(sock_fd, src, n, 0);
                if (sent < 0) {
                    _LOG_W("send() failed, closing socket");
                    close(sock_fd);
                    sock_fd = -1;
                }
            }
        }
    }
    // not reached
}

// ---- Public API ----
bool logger_init(const char *host, uint16_t port, size_t buf_size) {
    _LOG_D("Start of function");
    if (buf_size < 1024) buf_size = 1024;
    log_buffer = (char*)malloc(buf_size);
    if (!log_buffer) { _LOG_D("Exiting function"); return false; }
    buffer_size = buf_size;
    buffer_pos = 0;

    buffer_mutex = xSemaphoreCreateMutex();
    if (!buffer_mutex) { _LOG_D("Exiting function"); return false; }

    // Allocate dedicated send buffer
    send_buffer = (char*)malloc(buf_size);
    if (!send_buffer) { _LOG_W("No send buffer; reverting to direct send (may race)"); send_buffer_size = 0; }
    else send_buffer_size = buf_size;

    // Copy connection target
    if (host && host[0]) {
        strncpy(s_host, host, sizeof(s_host)-1);
        s_host[sizeof(s_host)-1] = 0;
        s_port = port;
        _LOG_D("Logger target set to %s:%u", s_host, (unsigned)s_port);
    }

    // Install vprintf hook once, with safe previous pointer
    if (!s_hook_installed) {
        vprintf_like_t prev = esp_log_set_vprintf(tcp_logger_vprintf);
        if (prev && prev != tcp_logger_vprintf) {
            s_saved_vprintf = prev;
        }
        s_hook_installed = true;
    } else {
        _LOG_W("logger_init called again; hook already installed");
    }

    // Subscribe to IP/Wi-Fi changes (best effort)
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,  &net_event_handler, NULL, &s_ip_got_inst);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_LOST_IP, &net_event_handler, NULL, &s_ip_lost_inst);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &net_event_handler, NULL, &s_wifi_disc_inst);

    _LOG_D("Starting Send Task");
    _LOG_I("Queueing a new task");
    xTaskCreate(sender_task, "tcp_log_send", 4096, NULL, 5, &sender_task_handle);
    _LOG_D("Exiting function");
    return true;
}

void logger_flush(void) {
    _LOG_D("Start of function");
    if (sock_fd < 0) { _LOG_D("Exiting function"); return; }
    // Copy to local scratch to avoid race
    size_t n = 0;
    xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    n = buffer_pos;
    if (n > 0 && send_buffer && n <= send_buffer_size) {
        memcpy(send_buffer, log_buffer, n);
    }
    buffer_pos = 0;
    xSemaphoreGive(buffer_mutex);
    if (n > 0) {
        const char *src = (send_buffer && n <= send_buffer_size) ? send_buffer : log_buffer;
        ssize_t sent = send(sock_fd, src, n, 0);
        if (sent < 0) {
            _LOG_W("flush send() failed, closing socket");
            close(sock_fd);
            sock_fd = -1;
        }
    }
    _LOG_D("Exiting function");
}

void logger_stop(void) {
    _LOG_D("Start of function");
    // Restore original console vprintf
    if (s_saved_vprintf) esp_log_set_vprintf(s_saved_vprintf);
    s_saved_vprintf = NULL;
    s_hook_installed = false;

    // Unregister events
    if (s_ip_got_inst)  { esp_event_handler_instance_unregister(IP_EVENT,   IP_EVENT_STA_GOT_IP,  s_ip_got_inst);  s_ip_got_inst = NULL; }
    if (s_ip_lost_inst) { esp_event_handler_instance_unregister(IP_EVENT,   IP_EVENT_STA_LOST_IP, s_ip_lost_inst); s_ip_lost_inst = NULL; }
    if (s_wifi_disc_inst) { esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_disc_inst); s_wifi_disc_inst = NULL; }

    if (sender_task_handle) vTaskDelete(sender_task_handle);
    sender_task_handle = NULL;

    if (sock_fd >= 0) { close(sock_fd); sock_fd = -1; }
    if (buffer_mutex) { vSemaphoreDelete(buffer_mutex); buffer_mutex = NULL; }
    if (log_buffer)   { free(log_buffer); log_buffer = NULL; buffer_size = buffer_pos = 0; }
    if (send_buffer)  { free(send_buffer); send_buffer = NULL; send_buffer_size = 0; }

    _LOG_D("Exiting function");
}
