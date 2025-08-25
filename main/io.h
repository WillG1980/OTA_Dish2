#ifndef OTA_DISH_IO_H
#define OTA_DISH_IO_H

// One-GPIO-per-device mode (Preset C):
// - Switches (active-low): Start, Cancel, Delay, Quick Rinse
// - LEDs (active-high): status_washing, status_sensing, status_drying, status_clean, control_lock
//
// Reserved elsewhere: Relays=33,32,25,26,27  |  ADC=34

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Compile-time marker for this wiring profile ----
#define IO_ONEPIN_MODE 1

// ---- GPIO assignments (Preset C) ----
// Switches (INPUT, internal pull-up; switch -> GND)
#define IO_PIN_SW_START        GPIO_NUM_16
#define IO_PIN_SW_CANCEL       GPIO_NUM_17
#define IO_PIN_SW_DELAY        GPIO_NUM_18
#define IO_PIN_SW_QUICK_RINSE  GPIO_NUM_19

// LEDs (OUTPUT, active-high; GPIO -> LED -> series resistor -> GND)
#define IO_PIN_LED_STATUS_WASHING  GPIO_NUM_21
#define IO_PIN_LED_STATUS_SENSING  GPIO_NUM_22
#define IO_PIN_LED_STATUS_DRYING   GPIO_NUM_23
#define IO_PIN_LED_STATUS_CLEAN    GPIO_NUM_13
#define IO_PIN_LED_CONTROL_LOCK    GPIO_NUM_14

// ---- Logical identifiers ----
typedef enum {
  IO_LED_STATUS_WASHING = 0,
  IO_LED_STATUS_SENSING,
  IO_LED_STATUS_DRYING,
  IO_LED_STATUS_CLEAN,
  IO_LED_CONTROL_LOCK,
  IO_LED_COUNT
} io_led_t;

typedef enum {
  IO_SW_START = 0,
  IO_SW_CANCEL,
  IO_SW_DELAY,
  IO_SW_QUICK_RINSE,
  IO_SW_COUNT
} io_switch_t;

// ---- Public API ----

/**
 * @brief Configure all switch/LED GPIOs for Preset C.
 *        - Switches: inputs with internal pull-ups
 *        - LEDs: outputs, initialized OFF
 */
esp_err_t io_init_onepin(void);

/** @brief Set LED state (true=ON). */
void io_led_set(io_led_t led, bool on);

/** @brief Toggle an LED. */
void io_led_toggle(io_led_t led);

/** @brief Convenience helpers. */
static inline void io_led_on (io_led_t led) { io_led_set(led, true);  }
static inline void io_led_off(io_led_t led) { io_led_set(led, false); }

/**
 * @brief Read a switch level (active-low). Returns true when pressed.
 *        (Debounce is the caller's responsibility if needed.)
 */
bool io_switch_pressed(io_switch_t sw);

/**
 * @brief Blink an LED by string name (case-insensitive).
 * @param name        One of: "status_washing","status_sensing","status_drying","status_clean","control_lock"
 * @param time_on_ms  ON duration in milliseconds (>=1)
 * @param time_off_ms OFF duration in milliseconds (>=1)
 * @param count       Number of ON pulses; 0 = blink forever (until another call for the same LED)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if name unknown/invalid.
 */
esp_err_t LED_Blink(const char *name, uint32_t time_on_ms, uint32_t time_off_ms, int count);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // OTA_DISH_IO_H
