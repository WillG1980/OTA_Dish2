#ifndef OTA_DISH_IO_H
#define OTA_DISH_IO_H

// One-GPIO-per-device mode (Preset C):
// - Switches (active-low): Start, Cancel, Delay, Quick Rinse
// - LEDs (active-high): status_washing, status_sensing, status_drying, status_clean, control_lock
//
// Relays in use elsewhere: 33, 32, 25, 26, 27
// ADC in use elsewhere:    34

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

// ---- Logical identifiers (stable across code) ----
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

/**
 * @brief Set LED state (true = ON, false = OFF).
 */
void io_led_set(io_led_t led, bool on);

/**
 * @brief Convenience helpers.
 */
static inline void io_led_on (io_led_t led) { io_led_set(led, true);  }
static inline void io_led_off(io_led_t led) { io_led_set(led, false); }

/**
 * @brief Toggle an LED.
 */
void io_led_toggle(io_led_t led);

/**
 * @brief Read a switch level (active-low): returns true when pressed.
 *        Debounce is the caller's responsibility (or use your poll task).
 */
bool io_switch_pressed(io_switch_t sw);

// (Optional) Simple poller you can start if you want edge logs/debounce internally.
// Provide in io.c if desired, or omit.
// void io_start_poll_task(UBaseType_t prio, uint32_t stack_bytes);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // OTA_DISH_IO_H
