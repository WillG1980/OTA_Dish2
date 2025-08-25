#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ----- Switch pins (active-low) -----
#define SW_START        GPIO_NUM_16
#define SW_CANCEL       GPIO_NUM_17
#define SW_DELAY        GPIO_NUM_18
#define SW_QUICK_RINSE  GPIO_NUM_19

// ----- LED pins (active-high) -----
#define LED_STATUS_WASHING   GPIO_NUM_21
#define LED_STATUS_SENSING   GPIO_NUM_22
#define LED_STATUS_DRYING    GPIO_NUM_23
#define LED_STATUS_CLEAN     GPIO_NUM_13
#define LED_CONTROL_LOCK     GPIO_NUM_14

static inline void led_set(gpio_num_t pin, bool on) {
  gpio_set_level(pin, on ? 1 : 0);
}

static inline bool sw_pressed(gpio_num_t pin) {
  // Active-low: pressed when level == 0
  return gpio_get_level(pin) == 0;
}

esp_err_t ui_pins_init(void) {
  // LEDs as outputs, start OFF
  const gpio_config_t led_cfg = {
    .pin_bit_mask = (1ULL<<LED_STATUS_WASHING) |
                    (1ULL<<LED_STATUS_SENSING) |
                    (1ULL<<LED_STATUS_DRYING)  |
                    (1ULL<<LED_STATUS_CLEAN)   |
                    (1ULL<<LED_CONTROL_LOCK),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  ESP_ERROR_CHECK(gpio_config(&led_cfg));
  led_set(LED_STATUS_WASHING, false);
  led_set(LED_STATUS_SENSING, false);
  led_set(LED_STATUS_DRYING,  false);
  led_set(LED_STATUS_CLEAN,   false);
  led_set(LED_CONTROL_LOCK,   false);

  // Switches as inputs with internal pull-ups
  const gpio_config_t sw_cfg = {
    .pin_bit_mask = (1ULL<<SW_START) | (1ULL<<SW_CANCEL) |
                    (1ULL<<SW_DELAY) | (1ULL<<SW_QUICK_RINSE),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  ESP_ERROR_CHECK(gpio_config(&sw_cfg));

  _LOG_I("UI pins ready: 4 switches (16,17,18,19), 5 LEDs (21,22,23,13,14)");
  return ESP_OK;
}

// Tiny poller example (debounce ~20 ms)
void ui_poll_task(void *arg) {
  const TickType_t dt = pdMS_TO_TICKS(10);
  uint8_t cnt_start=0,cnt_cancel=0,cnt_delay=0,cnt_quick=0;

  while (1) {
    cnt_start  = (sw_pressed(SW_START)       ? (cnt_start  < 2 ? cnt_start+1  : 2) : 0);
    cnt_cancel = (sw_pressed(SW_CANCEL)      ? (cnt_cancel < 2 ? cnt_cancel+1 : 2) : 0);
    cnt_delay  = (sw_pressed(SW_DELAY)       ? (cnt_delay  < 2 ? cnt_delay+1  : 2) : 0);
    cnt_quick  = (sw_pressed(SW_QUICK_RINSE) ? (cnt_quick  < 2 ? cnt_quick+1  : 2) : 0);

    if (cnt_start  == 2) { _LOG_I("Switch 'Start' pressed");       /* handle */ cnt_start=3;  }
    if (cnt_cancel == 2) { _LOG_I("Switch 'Cancel' pressed");      /* handle */ cnt_cancel=3; }
    if (cnt_delay  == 2) { _LOG_I("Switch 'Delay' pressed");       /* handle */ cnt_delay=3;  }
    if (cnt_quick  == 2) { _LOG_I("Switch 'Quick Rinse' pressed"); /* handle */ cnt_quick=3;  }

    vTaskDelay(dt);
  }
}
