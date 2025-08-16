#ifndef LOCAL_WIFI_H
#define LOCAL_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize Wi-Fi (STA) and connect with fallback:
 * 1) If a previously selected credential is saved in NVS, use only that one.
 * 2) Otherwise try WIFI_SSID_REAL, then WIFI_SSID_WOKWI.
 *    The first that connects becomes the only one used for reconnects,
 *    and is saved to NVS.
 *
 * Returns ESP_OK on successful connection (got IP).
 */
esp_err_t local_wifi_init_and_connect(void);

/** Returns true if currently connected (i.e., got IP and not disconnected). */
bool is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* LOCAL_WIFI_H */
