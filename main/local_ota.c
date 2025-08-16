#include "dishwasher_programs.h" // For BASE_URL and VERSION
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "local_wifi.h"

#ifndef TAG
#define TAG PROJECT_NAME
#endif
#ifndef BASE_URL
#define BASE_URL "http://house.sjcnu.com"
#endif

// Forward declarations
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);
static void _get_ota(const char *url);
extern status_struct ActiveStatus;

/**
 * Performs an HTTP/HTTPS GET request and returns the body in a malloc'd buffer.
 * Caller must free() the buffer. Logs status code and content length.
 * 
 * @param url         The full URL to fetch.
 * @param out_len     If not NULL, receives length of returned body.
 * @param out_status  If not NULL, receives HTTP status code.
 * @return            Pointer to malloc'd response body, or NULL on error.
 */void check_and_perform_ota(void) {
    // Optional: Log current DNS
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_dns_info_t dns_info;
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &dns_info.ip.u_addr.ip4.addr, ip_str, sizeof(ip_str));
            _LOG_I("Main DNS is %s", ip_str);
        }
    }
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
        _LOG_E("Firmware server returned HTTP %d | response: %s", status,response);
        free(response);
        return;
    }

    // Trim trailing whitespace/newlines
    for (int i = resp_len - 1; i >= 0 &&
         (response[i] == '\n' || response[i] == '\r' || response[i] == ' '); i--) {
        response[i] = '\0';
    }

    _LOG_I("Firmware server replied: '%s'", response);

    if (strcasecmp(response, "OK - Firmware up-to-date") == 0) {
        _LOG_I("Firmware is up-to-date");
    } else if (strncasecmp(response, "http", 4) == 0) {
        _LOG_I("New firmware URL provided, starting OTA: %s", response);
        setCharArray(ActiveStatus.Program,"Updating");

        _get_ota(response);
    } else {
        _LOG_W("Unexpected response from server %s",response);
    }

    free(response);
}


static void _get_ota(const char *url) {
    esp_http_client_config_t config = {
        .url = url,
    };

    // Attach cert bundle if HTTPS
    if (strncasecmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    _LOG_I("Starting OTA update from %s...", url);
    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK) {
        _LOG_I("OTA update successful. Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(10000000));
         esp_restart(); // Uncomment for production
    } else {
        _LOG_E("OTA update failed: %s", esp_err_to_name(ret));
    }
}
