#include "dishwasher_programs.h" // For BASE_URL, VERSION, PROJECT_NAME
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <strings.h>   // for strncasecmp
#include <stdlib.h>
#include <arpa/inet.h>

#include "local_wifi.h"

#ifndef TAG
#define TAG PROJECT_NAME
#endif

#ifndef BASE_URL
#define BASE_URL "http://house.sjcnu.com"
#endif

// If VERSION might be numeric, stringify safely
#ifndef STR_HELPER
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#endif

#ifndef VERSION_STR
#ifdef VERSION
#define VERSION_STR STR(VERSION)
#else
#define VERSION_STR "0"
#endif
#endif

// --- Forward declarations ---
extern status_struct ActiveStatus;
char* _http_get(const char *url, int *out_len, int *out_status);  // defined elsewhere

static void  _get_ota(const char *url);                           // spawns task
static void  _get_ota_task(void *param);                          // task entry

// Track single OTA task instance
static TaskHandle_t s_ota_task = NULL;

// Optional: small event handler (currently unused)
// static esp_err_t _http_event_handler(esp_http_client_event_t *evt) { return ESP_OK; }

void check_and_perform_ota(void) {
    // Log DNS server (IPv4) if available
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_dns_info_t dns_info;
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
            dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
            char ip_str[INET_ADDRSTRLEN] = {0};
            struct in_addr in4 = { .s_addr = dns_info.ip.u_addr.ip4.addr };
            if (inet_ntop(AF_INET, &in4, ip_str, sizeof(ip_str))) {
                _LOG_I("Main DNS is %s", ip_str);
            }
        }
    }

    // MAC → hex
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char request_url[512];
    snprintf(request_url, sizeof(request_url),
             "%s/firmware.php?version=%s&mac=%02X%02X%02X%02X%02X%02X&project_name=%s",
             BASE_URL, VERSION,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], TAG);

    int resp_len = 0;
    int status = 0;
    char *response = _http_get(request_url, &resp_len, &status);

    if (!response) {
        _LOG_E("No response from firmware server");
        return;
    }

    if (status != 200) {
        _LOG_E("Firmware server returned HTTP %d | response: %.*s", status, resp_len, response);
        free(response);
        return;
    }

    // Trim trailing whitespace/newlines
    while (resp_len > 0) {
        char c = response[resp_len - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            response[--resp_len] = '\0';
        } else {
            break;
        }
    }

    _LOG_I("Firmware server replied: '%s'", response);

    // Accept ANY string starting with "OK - "
    if (strncasecmp(response, "OK - ", 5) == 0) {
        setCharArray(ActiveStatus.FirmwareStatus,"Up To Date");
        _LOG_I("Firmware is up-to-date");
        free(response);
        return;
    }

    // Otherwise, expect a URL to the new firmware
    if (strncasecmp(response, "http", 4) == 0) {
        _LOG_I("New firmware URL provided, starting OTA in background: %s", response);
        setCharArray(ActiveStatus.Program, "Updating");

        // Duplicate URL so we can free response
        char *url_copy = strdup(response);
        if (!url_copy) {
            _LOG_E("strdup failed");
            free(response);
            return;
        }
        free(response);

        _get_ota(url_copy); // spawns task and takes ownership of url_copy
        return;
    }
    setCharArray(ActiveStatus.FirmwareStatus,"Server Error");

    _LOG_W("Unexpected response from server: %s", response);
    free(response);
}

// Spawn the OTA task with 16 KB stack
static void _get_ota(const char *url_ownership) {
    if (s_ota_task) {
        _LOG_W("OTA task already running; ignoring new request");
        free((void*)url_ownership);
        return;
    }

    // Stack size is in words; convert 16KB bytes → words
    #define OTA_TASK_STACK_WORDS ((16 * 1024) / sizeof(StackType_t))
    const UBaseType_t prio = tskIDLE_PRIORITY + 5;

    BaseType_t ok = xTaskCreatePinnedToCore(
        _get_ota_task,
        "ota_task",
        OTA_TASK_STACK_WORDS,
        (void*)url_ownership,   // task owns and frees this
        prio,
        &s_ota_task,
        tskNO_AFFINITY
    );

    if (ok != pdPASS) {
        _LOG_E("Failed to create OTA task");
        free((void*)url_ownership);
        s_ota_task = NULL;
    } else {
        _LOG_I("OTA task created");
    }
}

static void _get_ota_task(void *param) {
    char *url = (char*)param;  // ownership transferred to task

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 15000,
        // .event_handler = _http_event_handler,
    };

    // Attach cert bundle if HTTPS
    if (strncasecmp(url, "https://", 8) == 0) {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };
    setCharArray(ActiveStatus.FirmwareStatus,"Starting Update");
    _LOG_I("Starting OTA update from %s ...", url);

    esp_err_t ret = esp_https_ota(&ota_cfg);
    _LOG_I("Flash finished");
    if (ret == ESP_OK) {
        setCharArray(ActiveStatus.FirmwareStatus,"Pending Reboot");        
        // 1 minutes
        _LOG_I("Rebooting in 1 minute");
        vTaskDelay(pdMS_TO_TICKS(1 * MIN*SEC));
        _LOG_I("Rebooting now after OTA delay.");
        free(url);
        s_ota_task = NULL;
        esp_restart(); // never returns
    } else {
        setCharArray(ActiveStatus.FirmwareStatus,"Firmware Failed");        
        _LOG_E("OTA update failed: %s", esp_err_to_name(ret));
        free(url);
        s_ota_task = NULL;
        vTaskDelete(NULL);
    }
}
