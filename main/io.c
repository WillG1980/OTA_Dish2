#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dishwasher_programs.h"

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

static inline void io_led_set(gpio_num_t pin, bool on) {
  gpio_set_level(pin, on ? 1 : 0);
}

static inline bool sw_pressed(gpio_num_t pin) {
  // Active-low: pressed when level == 0
  return gpio_get_level(pin) == 0;
}

#include <string.h>
#include <strings.h> // strcasecmp
#include "io.h"

// --- name -> enum map (Preset C names) ---
static const struct { const char *name; io_led_t id; } s_led_name_map[] = {
  {"status_washing", IO_LED_STATUS_WASHING},
  {"status_sensing", IO_LED_STATUS_SENSING},
  {"status_drying",  IO_LED_STATUS_DRYING},
  {"status_clean",   IO_LED_STATUS_CLEAN},
  {"control_lock",   IO_LED_CONTROL_LOCK},
};

static bool name_to_led(const char *name, io_led_t *out) {
  if (!name || !out) return false;
  for (size_t i = 0; i < sizeof(s_led_name_map)/sizeof(s_led_name_map[0]); ++i) {
    if (strcasecmp(name, s_led_name_map[i].name) == 0) {
      *out = s_led_name_map[i].id;
      return true;
    }
  }
  return false;
}

// --- per-LED blink control ---
typedef struct {
  io_led_t   led;
  uint32_t   on_ms;
  uint32_t   off_ms;
  int        count;     // 0 = infinite
} blink_args_t;

static TaskHandle_t s_blink_task[IO_LED_COUNT] = {0};
static volatile bool s_blink_stop[IO_LED_COUNT] = {0};

static void blink_task(void *pv) {
  blink_args_t a = *(blink_args_t *)pv;
  free(pv); // allocated in LED_Blink
  const TickType_t on_ticks  = pdMS_TO_TICKS(a.on_ms  ? a.on_ms  : 1);
  const TickType_t off_ticks = pdMS_TO_TICKS(a.off_ms ? a.off_ms : 1);

  _LOG_D("LED_Blink start: led=%d on=%ums off=%ums count=%d", (int)a.led, (unsigned)a.on_ms, (unsigned)a.off_ms, a.count);

  int remaining = a.count;
  while (!s_blink_stop[a.led] && (remaining != 0)) {
    // ON
    io_led_set(a.led, true);
    vTaskDelay(on_ticks);
    if (s_blink_stop[a.led]) break;
    // OFF
    io_led_set(a.led, false);
    vTaskDelay(off_ticks);
    if (remaining > 0) remaining--;
  }

  // Ensure LED ends OFF when finished/cancelled
  io_led_set(a.led, false);

  _LOG_D("LED_Blink end: led=%d", (int)a.led);
  s_blink_task[a.led] = NULL;
  s_blink_stop[a.led] = false;
  vTaskDelete(NULL);
}

esp_err_t LED_Blink(const char *name, uint32_t time_on_ms, uint32_t time_off_ms, int count) {
  io_led_t led;
  if (!name_to_led(name, &led)) {
    _LOG_E("LED_Blink: unknown LED name '%s'", name ? name : "(null)");
    return ESP_ERR_INVALID_ARG;
  }
  if (time_on_ms == 0 && time_off_ms == 0) {
    _LOG_E("LED_Blink: both times are 0");
    return ESP_ERR_INVALID_ARG;
  }

  // Cancel existing blinker on this LED, if any
  if (s_blink_task[led]) {
    s_blink_stop[led] = true;
    // Give it a moment to clean up
    for (int i = 0; i < 50 && s_blink_task[led]; ++i) vTaskDelay(pdMS_TO_TICKS(2)); // up to ~100ms
  }

  // Prepare args
  blink_args_t *args = (blink_args_t *)malloc(sizeof(blink_args_t));
  if (!args) return ESP_ERR_NO_MEM;
  args->led    = led;
  args->on_ms  = (time_on_ms  == 0) ? 1 : time_on_ms;
  args->off_ms = (time_off_ms == 0) ? 1 : time_off_ms;
  args->count  = count; // 0 == infinite

  s_blink_stop[led] = false;
  BaseType_t ok = xTaskCreate(blink_task, "led_blink", 2048, args, 5, &s_blink_task[led]);
  if (ok != pdPASS) {
    free(args);
    s_blink_task[led] = NULL;
    _LOG_E("LED_Blink: failed to create task");
    return ESP_FAIL;
  }
  return ESP_OK;
}
void LED_Blink_Cancel(const char *name) {
  io_led_t led;
  if (!name_to_led(name, &led)) {
    _LOG_E("LED_Blink_Cancel: unknown LED name '%s'", name ? name : "(null)");
    return;
  }
  if (s_blink_task[led]) {
    s_blink_stop[led] = true;
    // Give it a moment to clean up
    for (int i = 0; i < 50 && s_blink_task[led]; ++i) vTaskDelay(pdMS_TO_TICKS(2)); // up to ~100ms
  }
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
#include "io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// One-shot: test every LED for 5s each (then stop)
void io_test_all_leds_once(void) {
  static const struct { io_led_t id; const char *name; } L[] = {
    {IO_LED_STATUS_WASHING, "status_washing"},
    {IO_LED_STATUS_SENSING, "status_sensing"},
    {IO_LED_STATUS_DRYING,  "status_drying"},
    {IO_LED_STATUS_CLEAN,   "status_clean"},
    {IO_LED_CONTROL_LOCK,   "control_lock"},
  };

  for (size_t i = 0; i < sizeof(L)/sizeof(L[0]); ++i) {
    _LOG_I("LED test: %s ON (5s)", L[i].name);
    io_led_set(L[i].id, true);
    vTaskDelay(pdMS_TO_TICKS(5000));

    io_led_set(L[i].id, false);
    _LOG_I("LED test: %s OFF", L[i].name);

    vTaskDelay(pdMS_TO_TICKS(250)); // small gap before next LED
  }

  _LOG_I("LED test: complete.");
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
