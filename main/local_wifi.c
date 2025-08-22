#include "local_wifi.h"
#include <string.h>
#include <stdbool.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "dishwasher_programs.h"

/* =======================
 *  Compile-time defaults
 * ======================= */
// Your "real" network – set via CMake:
// add_compile_definitions(WIFI_SSID_REAL=\"House619\" WIFI_PASS_REAL=\"Wifi6860\")
#ifndef WIFI_SSID_REAL
#define WIFI_SSID_REAL "YOUR_REAL_SSID"
#endif
#ifndef WIFI_PASS_REAL
#define WIFI_PASS_REAL "YOUR_REAL_PASSWORD"
#endif

// Fallback (Wokwi) network
#ifndef WIFI_SSID_WOKWI
#define WIFI_SSID_WOKWI "Wokwi-GUEST"
#endif
#ifndef WIFI_PASS_WOKWI
#define WIFI_PASS_WOKWI ""
#endif

#ifndef TAG
#define TAG "LOCAL_WIFI"
#endif

/* =======================
 *  Config & State
 * ======================= */
#define WIFI_CONNECT_TIMEOUT_MS   15000   // per-credential wait (IP or fail)
#define WIFI_RETRIES_PER_CRED     5       // before we declare "fail" for that cred

// Persist the chosen credential to NVS so reboots keep using it.
// Set to 0 if you don't want persistence.
#define PERSIST_SELECTED_CRED_TO_NVS 1
#define NVS_NS_WIFI   "wifi"
#define NVS_KEY_CRED  "selected_cred"

typedef enum {
    CRED_UNKNOWN = 0,
    CRED_REAL    = 1,
    CRED_WOKWI   = 2,
} selected_cred_t;

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_wifi_netif = NULL;

static volatile bool s_connected = false;          // true once we have IP
static volatile bool s_blocking_wait_active = false;
static int s_retries_remaining = 0;

static selected_cred_t s_selected_cred = CRED_UNKNOWN; // set after first success

// EventGroup bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* =======================
 *  Helpers
 * ======================= */
static void _set_sta_config(const char *ssid, const char *pass) {
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);

    // If password empty, allow OPEN networks (authmode default is fine).
    // Otherwise WPA/WPA2/etc works as usual.

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

static void _save_selected_cred_to_nvs(selected_cred_t cred) {
#if PERSIST_SELECTED_CRED_TO_NVS
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WIFI, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_CRED, (uint8_t)cred);
        nvs_commit(h);
        nvs_close(h);
    }
#endif
}

static selected_cred_t _load_selected_cred_from_nvs(void) {
#if PERSIST_SELECTED_CRED_TO_NVS
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS_WIFI, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_KEY_CRED, &v) == ESP_OK) {
            nvs_close(h);
            if (v == CRED_REAL || v == CRED_WOKWI) return (selected_cred_t)v;
        }
        nvs_close(h);
    }
#endif
    return CRED_UNKNOWN;
}

/* =======================
 *  Event Handler
 * ======================= */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            _LOG_I( "WIFI_EVENT_STA_START → esp_wifi_connect()");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

            if (s_blocking_wait_active) {
                if (s_retries_remaining-- > 0) {
                    _LOG_W( "Disconnected; retrying… (%d left)", s_retries_remaining);
                    esp_wifi_connect();
                } else {
                    _LOG_E( "Giving up on this credential.");
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            } else {
                // After we've "locked" a credential, always retry the SAME config.
                _LOG_W( "Disconnected; retrying current credential…");
                esp_wifi_connect();
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* =======================
 *  Core connect routine
 * ======================= */
static esp_err_t _try_credential(const char *ssid,
                                 const char *pass,
                                 int retries,
                                 uint32_t timeout_ms)
{
    _LOG_I( "Trying SSID: \"%s\"", ssid);

    _set_sta_config(ssid, pass);

    // Start or restart STA as needed
    static bool wifi_started = false;
    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    } else {
        // Force a reconnect cycle for the new config
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_wifi_connect();
    }

    s_retries_remaining = retries;
    s_blocking_wait_active = true;

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,   // clear on exit
        pdFALSE,  // wait for any bit
        pdMS_TO_TICKS(timeout_ms)
    );

    s_blocking_wait_active = false;

    if (bits & WIFI_CONNECTED_BIT) {
        _LOG_I( "Connected to \"%s\"", ssid);
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        _LOG_E( "Failed to connect to \"%s\"", ssid);
        return ESP_FAIL;
    }

    // Timeout without explicit fail bit (e.g., no events fired)
    _LOG_E( "Timeout waiting for connection to \"%s\"", ssid);
    return ESP_ERR_TIMEOUT;
}

/* =======================
 *  Public API
 * ======================= */
esp_err_t local_wifi_init_and_connect(void)
{
    // One-time system init (idempotent)
    esp_err_t err;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!s_wifi_netif) {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    // Create event group & register handlers once
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    }

    // If we previously selected a credential (NVS), use only that.
    s_selected_cred = _load_selected_cred_from_nvs();
    if (s_selected_cred == CRED_REAL) {
        if (_try_credential(WIFI_SSID_REAL, WIFI_PASS_REAL, WIFI_RETRIES_PER_CRED, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK) {
            _LOG_I( "Using saved REAL credentials.");
            return ESP_OK;
        }
        _LOG_W( "Saved REAL credential failed; will still stay on REAL for reconnects.");
        return ESP_FAIL;
    } else if (s_selected_cred == CRED_WOKWI) {
        if (_try_credential(WIFI_SSID_WOKWI, WIFI_PASS_WOKWI, WIFI_RETRIES_PER_CRED, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK) {
            _LOG_I( "Using saved WOKWI credentials.");
            return ESP_OK;
        }
        _LOG_W( "Saved WOKWI credential failed; will still stay on WOKWI for reconnects.");
        return ESP_FAIL;
    }

    // No saved choice: REAL → WOKWI
    if (_try_credential(WIFI_SSID_REAL, WIFI_PASS_REAL, WIFI_RETRIES_PER_CRED, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK) {
        s_selected_cred = CRED_REAL;
        _save_selected_cred_to_nvs(s_selected_cred);
        return ESP_OK;
    }

    _LOG_W( "REAL failed; falling back to WOKWI…");
    if (_try_credential(WIFI_SSID_WOKWI, WIFI_PASS_WOKWI, WIFI_RETRIES_PER_CRED, WIFI_CONNECT_TIMEOUT_MS) == ESP_OK) {
        s_selected_cred = CRED_WOKWI;
        _save_selected_cred_to_nvs(s_selected_cred);
        return ESP_OK;
    }

    _LOG_E( "Both credentials failed.");
    return ESP_FAIL;
}

bool is_connected(void) {
    return s_connected;
}
