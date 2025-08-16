#ifndef LOCAL_WIFI_H
#define LOCAL_WIFI_H

<<<<<<< HEAD
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
=======
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

// Event bits
#define WIFI_CONNECTED_BIT BIT0

// Connection timeout in ticks
#define WIFI_FAIL_TIMEOUT pdMS_TO_TICKS(5000)  // 5 seconds

// Wi-Fi credentials
#define WIFI_SSID_REAL   "House619"
#define WIFI_PASS_REAL   "Wifi6860"
#define WIFI_SSID_WOKWI  "Wokwi-GUEST"
#define WIFI_PASS_WOKWI  ""

// Tag for logging
extern const char *TAG;

/**
 * @brief Initialize Wi-Fi in STA mode, attempt to connect to real or simulator SSID.
 * 
 * Tries WIFI_SSID_REAL first, then WIFI_SSID_WOKWI as a fallback.
 * Sets internal flag to indicate if running on simulator.
 */
void wifi_init_sta(void);

/**
 * @brief Check if currently connected to a Wi-Fi network.
 * 
 * @return true if connected, false otherwise.
 */
bool is_connected(void);

/**
 * @brief Check if the device is running in simulator mode (Wokwi).
 * 
 * @return true if using simulator SSID, false if connected to real Wi-Fi.
 */
bool is_simulator(void);

#endif // LOCAL_WIFI_H
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
