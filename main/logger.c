#include "logger.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------- Config ----------
#ifndef LOGGER_QUEUE_DEPTH
#define LOGGER_QUEUE_DEPTH 64
#endif

#ifndef LOGGER_PKT_MAX
#define LOGGER_PKT_MAX 256      // max bytes copied from each ESP_LOG line
#endif

#ifndef LOGGER_TASK_STACK
#define LOGGER_TASK_STACK 4096
#endif

#ifndef LOGGER_TASK_PRIO
#define LOGGER_TASK_PRIO (tskIDLE_PRIORITY + 2)
#endif

// ---------- State ----------
typedef struct { uint16_t len; char buf[LOGGER_PKT_MAX]; } logpkt_t;

static QueueHandle_t   s_q          = NULL;
static TaskHandle_t    s_task       = NULL;
static vprintf_like_t  s_prev_vprintf = NULL;

static bool            s_use_udp    = false;
static bool            s_use_tcp    = false;

static int             s_udp_sock   = -1;
static struct sockaddr_storage s_udp_dst;
static socklen_t       s_udp_dstlen = 0;

static int             s_tcp_sock   = -1;
static struct sockaddr_storage s_tcp_dst;
static socklen_t       s_tcp_dstlen = 0;

static uint32_t        s_drop_count = 0;
static volatile bool   s_running    = false;

// ---------- helpers (NO ESP_LOG here; avoid recursion) ----------
static void close_sock(int *ps) { if (ps && *ps >= 0) { close(*ps); *ps = -1; } }

static int resolve_host(const char *host, uint16_t port,
                        struct sockaddr_storage *out, socklen_t *outlen) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_INET;        // IPv4 only; change to AF_UNSPEC for v6
    hints.ai_socktype = SOCK_DGRAM;     // not strict; we just need addr/port

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

    memset(out, 0, sizeof(*out));
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *outlen = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static void ensure_udp_socket(void) {
    if (!s_use_udp || s_udp_sock >= 0) return;
    s_udp_sock = socket(s_udp_dst.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_sock < 0) return;

    // Optional send timeout (avoid blocking forever)
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_udp_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void tcp_disconnect(void) { close_sock(&s_tcp_sock); }

static void ensure_tcp_connected(void) {
    if (!s_use_tcp) return;
    if (s_tcp_sock >= 0) return;

    s_tcp_sock = socket(s_tcp_dst.ss_family, SOCK_STREAM, IPPROTO_IP);
    if (s_tcp_sock < 0) return;

    // Small timeouts so we can break and reconnect
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_tcp_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s_tcp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(s_tcp_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(s_tcp_sock, (struct sockaddr *)&s_tcp_dst, s_tcp_dstlen) < 0) {
        tcp_disconnect();
    }
}

// The vprintf hook: mirror to queue, but always forward to original first.
static int logger_vprintf(const char *fmt, va_list ap) {
    int rc = 0;
    if (s_prev_vprintf) {
        va_list ap2; va_copy(ap2, ap);
        rc = s_prev_vprintf(fmt, ap2);
        va_end(ap2);
    } else {
        rc = vprintf(fmt, ap);
    }

    if (s_q) {
        logpkt_t pkt;
        pkt.len = (uint16_t)vsnprintf(pkt.buf, sizeof(pkt.buf), fmt, ap);
        if (pkt.len > sizeof(pkt.buf)) pkt.len = sizeof(pkt.buf);
        if (pkt.len > 0) {
            if (xQueueSend(s_q, &pkt, 0) != pdTRUE) {
                // queue full: drop
                s_drop_count++;
            }
        }
    }
    return rc;
}

static void logger_task(void *arg) {
    (void)arg;
    s_running = true;
    TickType_t last_reconn = xTaskGetTickCount();

    for (;;) {
        // periodic (1s) connect/keepalive attempts for TCP, and UDP socket creation
        if ((xTaskGetTickCount() - last_reconn) >= pdMS_TO_TICKS(1000)) {
            ensure_udp_socket();
            ensure_tcp_connected();
            last_reconn = xTaskGetTickCount();
        }

        logpkt_t pkt;
        if (xQueueReceive(s_q, &pkt, pdMS_TO_TICKS(200)) == pdTRUE) {
            // Try UDP
            if (s_use_udp && s_udp_sock >= 0) {
                int r = sendto(s_udp_sock, pkt.buf, pkt.len, 0,
                               (struct sockaddr*)&s_udp_dst, s_udp_dstlen);
                (void)r; // ignore errors; we'll retry later
            }
            // Try TCP
            if (s_use_tcp) {
                if (s_tcp_sock < 0) ensure_tcp_connected();
                if (s_tcp_sock >= 0) {
                    int sent = send(s_tcp_sock, pkt.buf, pkt.len, 0);
                    if (sent < 0) tcp_disconnect(); // reconnect next tick
                }
            }
        }

        // graceful exit
        if (!s_running && uxQueueMessagesWaiting(s_q) == 0) break;
    }

    tcp_disconnect();
    close_sock(&s_udp_sock);
    vTaskDelete(NULL);
}

// ---------- API ----------
esp_err_t logger_init(const char *host, uint16_t port,int buffer) {
    return logger_init_net( host, port, true,false);
}

esp_err_t logger_init_net(const char *host, uint16_t port, bool use_udp, bool use_tcp) {
    if (!host || (!use_udp && !use_tcp)) return ESP_ERR_INVALID_ARG;
    if (s_task) return ESP_OK; // already running

    // Resolve once (same numeric port can be used for both UDP & TCP)
    if (resolve_host(host, port, &s_udp_dst, &s_udp_dstlen) != 0) return ESP_ERR_INVALID_ARG;
    s_tcp_dst   = s_udp_dst;
    s_tcp_dstlen= s_udp_dstlen;

    s_use_udp = use_udp;
    s_use_tcp = use_tcp;

    s_q = xQueueCreate(LOGGER_QUEUE_DEPTH, sizeof(logpkt_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreatePinnedToCore(
        logger_task, "logger_net", LOGGER_TASK_STACK, NULL,
        LOGGER_TASK_PRIO, &s_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        vQueueDelete(s_q); s_q = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Hook vprintf last (after task/queue exist)
    s_prev_vprintf = esp_log_set_vprintf(logger_vprintf);
    return ESP_OK;
}

void logger_shutdown_net(void) {
    if (!s_task) return;
    // Unhook first so we stop enqueuing
    if (s_prev_vprintf) { esp_log_set_vprintf(s_prev_vprintf); s_prev_vprintf = NULL; }
    // Signal task to drain & exit
    s_running = false;
    // Wait a bit for queue drain
    vTaskDelay(pdMS_TO_TICKS(300));
    // Force close if still there
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_q)    { vQueueDelete(s_q);   s_q = NULL; }
    tcp_disconnect();
    close_sock(&s_udp_sock);
}

uint32_t logger_get_drop_count(void) { return s_drop_count; }
