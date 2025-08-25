// io.c — one-GPIO-per-device (Preset C)

#include <stdlib.h>
#include <string.h>
#include <strings.h> // strcasecmp

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dishwasher_programs.h"
#include "io.h"

// ===== Pin maps (enum -> GPIO) =====
static const gpio_num_t LED_GPIO[IO_LED_COUNT] = {
  IO_PIN_LED_STATUS_WASHING, // IO_LED_STATUS_WASHING
  IO_PIN_LED_STATUS_SENSING, // IO_LED_STATUS_SENSING
  IO_PIN_LED_STATUS_DRYING,  // IO_LED_STATUS_DRYING
  IO_PIN_LED_STATUS_CLEAN,   // IO_LED_STATUS_CLEAN
  IO_PIN_LED_CONTROL_LOCK,   // IO_LED_CONTROL_LOCK
};

static const gpio_num_t SW_GPIO[IO_SW_COUNT] = {
  IO_PIN_SW_START,        // IO_SW_START
  IO_PIN_SW_CANCEL,       // IO_SW_CANCEL
  IO_PIN_SW_DELAY,        // IO_SW_DELAY
  IO_PIN_SW_QUICK_RINSE,  // IO_SW_QUICK_RINSE
};

// ===== Basic LED/Switch helpers =====
void io_led_set(io_led_t led, bool on) {
  if ((int)led < 0 || led >= IO_LED_COUNT) return;
  gpio_set_level(LED_GPIO[led], on ? 1 : 0);
}

void io_led_toggle(io_led_t led) {
  if ((int)led < 0 || led >= IO_LED_COUNT) return;
  int cur = gpio_get_level(LED_GPIO[led]);
  gpio_set_level(LED_GPIO[led], !cur);
}

bool io_switch_pressed(io_switch_t sw) {
  if ((int)sw < 0 || sw >= IO_SW_COUNT) return false;
  // Active-low: pressed when 0
  return gpio_get_level(SW_GPIO[sw]) == 0;
}

// ===== Initialization =====
esp_err_t io_init_onepin(void) {
  // LEDs as outputs (start OFF)
  const uint64_t led_mask =
      (1ULL << IO_PIN_LED_STATUS_WASHING) |
      (1ULL << IO_PIN_LED_STATUS_SENSING) |
      (1ULL << IO_PIN_LED_STATUS_DRYING)  |
      (1ULL << IO_PIN_LED_STATUS_CLEAN)   |
      (1ULL << IO_PIN_LED_CONTROL_LOCK);

  gpio_config_t led_cfg = {
    .pin_bit_mask = led_mask,
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE
  };
  ESP_ERROR_CHECK(gpio_config(&led_cfg));
  for (int i = 0; i < IO_LED_COUNT; ++i) gpio_set_level(LED_GPIO[i], 0);

  // Switches as inputs with internal pull-ups
  const uint64_t sw_mask =
      (1ULL << IO_PIN_SW_START) |
      (1ULL << IO_PIN_SW_CANCEL) |
      (1ULL << IO_PIN_SW_DELAY) |
      (1ULL << IO_PIN_SW_QUICK_RINSE);

  gpio_config_t sw_cfg = {
    .pin_bit_mask = sw_mask,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE
  };
  ESP_ERROR_CHECK(gpio_config(&sw_cfg));

  _LOG_I("UI pins ready: 4 switches (16,17,18,19), 5 LEDs (21,22,23,13,14)");
  return ESP_OK;
}

// ===== Name lookup for blink API =====
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

// ===== Non-blocking per-LED blinker =====
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
  free(pv);
  const TickType_t on_ticks  = pdMS_TO_TICKS(a.on_ms  ? a.on_ms  : 1);
  const TickType_t off_ticks = pdMS_TO_TICKS(a.off_ms ? a.off_ms : 1);

  _LOG_D("LED_Blink start: led=%d on=%ums off=%ums count=%d",
         (int)a.led, (unsigned)a.on_ms, (unsigned)a.off_ms, a.count);

  int remaining = a.count;
  while (!s_blink_stop[a.led] && (remaining != 0)) {
    io_led_set(a.led, true);
    vTaskDelay(on_ticks);
    if (s_blink_stop[a.led]) break;
    io_led_set(a.led, false);
    vTaskDelay(off_ticks);
    if (remaining > 0) remaining--;
  }

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

  if (s_blink_task[led]) {
    s_blink_stop[led] = true;
    for (int i = 0; i < 50 && s_blink_task[led]; ++i) vTaskDelay(pdMS_TO_TICKS(2)); // ~100 ms max
  }

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

// Optional cancel helper (declare in io.h if you plan to call it from other files)
void LED_Blink_Cancel(const char *name) {
  io_led_t led;
  if (!name_to_led(name, &led)) {
    _LOG_E("LED_Blink_Cancel: unknown LED name '%s'", name ? name : "(null)");
    return;
  }
  if (s_blink_task[led]) {
    s_blink_stop[led] = true;
    for (int i = 0; i < 50 && s_blink_task[led]; ++i) vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ===== Test helpers =====
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
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  _LOG_I("LED test: complete.");
}

// Optional: a simple poller example (software debounce)
void ui_poll_task(void *arg) {
  const TickType_t dt = pdMS_TO_TICKS(10);
  uint8_t cnt_start=0,cnt_cancel=0,cnt_delay=0,cnt_quick=0;

  while (1) {
    cnt_start  = (io_switch_pressed(IO_SW_START)       ? (cnt_start  < 2 ? cnt_start+1  : 2) : 0);
    cnt_cancel = (io_switch_pressed(IO_SW_CANCEL)      ? (cnt_cancel < 2 ? cnt_cancel+1 : 2) : 0);
    cnt_delay  = (io_switch_pressed(IO_SW_DELAY)       ? (cnt_delay  < 2 ? cnt_delay+1  : 2) : 0);
    cnt_quick  = (io_switch_pressed(IO_SW_QUICK_RINSE) ? (cnt_quick  < 2 ? cnt_quick+1  : 2) : 0);

    if (cnt_start  == 2) { _LOG_I("Switch 'Start' pressed");       cnt_start=3;  }
    if (cnt_cancel == 2) { _LOG_I("Switch 'Cancel' pressed");      cnt_cancel=3; }
    if (cnt_delay  == 2) { _LOG_I("Switch 'Delay' pressed");       cnt_delay=3;  }
    if (cnt_quick  == 2) { _LOG_I("Switch 'Quick Rinse' pressed"); cnt_quick=3;  }

    vTaskDelay(dt);
  }
}
