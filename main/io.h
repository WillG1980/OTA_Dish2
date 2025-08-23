// io.h
// Public API for panel matrix LEDs and switches (ESP-IDF)

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========= Data types ========= */

typedef struct {
  const char *name;
  uint8_t row;      // panel wire id (1..14)
  uint8_t col;      // panel wire id (1..14)
  bool status;      // desired/current LED state
} LED_struct;

typedef struct {
  const char *name;
  uint8_t row;                // panel wire id (1..14)
  uint8_t col;                // panel wire id (1..14)
  bool Pressed_NOW;           // set true on the scan when a debounced press is first detected
  bool Pressed_Registered;    // latched until cleared by user code
} SWITCH_struct;

typedef enum {
  LED_OFF = 0,
  LED_ON  = 1,
  LED_TOGGLE = 2,
} led_cmd_t;

/* ========= Exposed tables & counts =========
   NOTE: Do not modify these arrays from user code; use the API below.
*/
extern LED_struct    LEDS[];
extern SWITCH_struct SWITCHES[];
extern const size_t  LED_COUNT;
extern const size_t  SWITCH_COUNT;

/* ========= Wire mapping ========= */

/** Bind a panel wire number to a specific ESP32 GPIO. */
esp_err_t Matrix_BindWire(uint8_t wire, gpio_num_t gpio_num);

/** Mark a panel wire as a fixed ground (no GPIO; permanently 0). */
esp_err_t Matrix_BindWireFixedGND(uint8_t wire);

/** Convenience: bind the default GPIO map from the harness comment (no I2C). */
void Panel_BindDefaultGPIOMap(void);

/* ========= Initialization =========
   Call _init_LED() and _init_Switch() once after binding wire→GPIO mappings.
   Either function will spawn the background matrix task if not already running.
*/
esp_err_t _init_LED(void);
esp_err_t _init_Switch(void);

/* ========= LED control ========= */

/** Set/toggle an LED by its name and update internal state. */
esp_err_t LED_Toggle(const char *name, led_cmd_t op);

/** Read current desired state (true if ON). */
bool LED_Get(const char *name);

/* ========= Switch helpers ========= */

/** Clear the Pressed_Registered latch for a named switch. */
void Switch_ClearRegistered(const char *name);

/** Atomically read-and-clear Pressed_Registered for a named switch. */
bool Switch_Consume(const char *name);

/** True while the switch is stably held (debounced state). */
bool Switch_IsHeld(const char *name);

/** True only on the scan when the press edge was detected (does not clear). */
bool Switch_PressedNow(const char *name);
void _test_leds(void);
#ifdef __cplusplus
} // extern "C"
#endif
