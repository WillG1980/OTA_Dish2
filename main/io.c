// io.c
// ESP-IDF matrix LED + switch driver for dishwasher front panel
// - Single background task multiplexes LED rows and scans switch rows.
// - Supports wire->GPIO mapping and "fixed GND" wires (no GPIO).
// - Implements your default harness map (see Panel_BindDefaultGPIOMap).
// - WDT-friendly: no busy-waits; switch scan duty-cycled to a short window each second.

#include "dishwasher_programs.h"
#include "io.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

// ====== CONFIG: timing & electrical (adjust to taste) ======
#define MATRIX_TASK_STACK      4096
#define MATRIX_TASK_PRIO       5
#define MATRIX_SCAN_HZ         500                     // overall scan frequency (LED refresh cadence)
#define DEBOUNCE_MS            30                      // debounce window
#define LED_ROW_ON_LEVEL       1                       // row drive level to light an LED
#define LED_COL_ON_LEVEL       0                       // col drive level to light an LED (e.g., sink)
#define SW_ROW_ACTIVE_LEVEL    1                       // row drive level while scanning switches
#define SW_COL_PRESSED_LEVEL   0                       // expected level on column when switch is pressed

// ---- Switch scan duty cycle: only scan during a small window each second ----
#define SW_SCAN_PERIOD_MS      1000    // duration of the cycle
#define SW_SCAN_WINDOW_MS      150     // active scanning window within the cycle

// ====== Wire map ======
#define MAX_WIRE 14

typedef enum {
  WIRE_UNUSED = 0,
  WIRE_GPIO   = 1,
  WIRE_FIXED_GND = 2,
} wire_kind_t;

typedef struct {
  int gpio;          // valid only if kind == WIRE_GPIO
  wire_kind_t kind;
} wire_map_t;

static wire_map_t s_wire_map[MAX_WIRE + 1] = {
  [0]  = { .gpio = -1, .kind = WIRE_UNUSED },
  [1]  = { .gpio = -1, .kind = WIRE_UNUSED },
  [2]  = { .gpio = -1, .kind = WIRE_UNUSED },
  [3]  = { .gpio = -1, .kind = WIRE_UNUSED },
  [4]  = { .gpio = -1, .kind = WIRE_UNUSED },
  [5]  = { .gpio = -1, .kind = WIRE_UNUSED },
  [6]  = { .gpio = -1, .kind = WIRE_UNUSED }, // skipped
  [7]  = { .gpio = -1, .kind = WIRE_UNUSED }, // skipped
  [8]  = { .gpio = -1, .kind = WIRE_UNUSED },
  [9]  = { .gpio = -1, .kind = WIRE_UNUSED },
  [10] = { .gpio = -1, .kind = WIRE_UNUSED },
  [11] = { .gpio = -1, .kind = WIRE_UNUSED }, // skipped
  [12] = { .gpio = -1, .kind = WIRE_UNUSED },
  [13] = { .gpio = -1, .kind = WIRE_UNUSED }, // skipped
  [14] = { .gpio = -1, .kind = WIRE_UNUSED }, // skipped
};

static inline bool wire_is_valid(uint8_t wire)     { return wire > 0 && wire <= MAX_WIRE; }
static inline bool wire_is_gpio(uint8_t wire)      { return wire_is_valid(wire) && s_wire_map[wire].kind == WIRE_GPIO; }
static inline bool wire_is_fixed_gnd(uint8_t wire) { return wire_is_valid(wire) && s_wire_map[wire].kind == WIRE_FIXED_GND; }
static inline int  wire_gpio(uint8_t wire)         { return wire_is_gpio(wire) ? s_wire_map[wire].gpio : -1; }

// ====== Active elements (trim set + extras that need no new wires) ======
LED_struct LEDS[] = {
  {"status_washing", 10, 1, false},
  {"status_sensing",  9, 3, false},
  {"status_drying",  10, 4, false},
  {"status_clean",    8, 5, false},

  // Useful extras (no new wires required)
  {"delay_1",         8, 3, false},
  {"delay_3",        10, 3, false},
  {"switch_4",       10, 5, false},
};

SWITCH_struct SWITCHES[] = {
  // Core
  {"Start",        12, 4, false, false},
  {"Cancel",       12, 2, false, false},
  // Useful extras
  {"Delay",        12, 3, false, false},
  {"Quick Rinse",  12, 5, false, false},
};

/* Available but intentionally omitted due to node collision (duplicate of status_sensing) */
// {"delay_2", 9, 3, false}

/* Counts exposed */
const size_t LED_COUNT    = sizeof(LEDS)    / sizeof(LEDS[0]);
const size_t SWITCH_COUNT = sizeof(SWITCHES)/ sizeof(SWITCHES[0]);

/* ---- Currently unavailable (requires skipped wires: 6,7,11,13,14)
   LED_struct:
     {"control_lcok", 8, 6, false}
     {"switch_1",    14, 4, false}
     {"switch_2",    14, 5, false}
     {"switch_3",    10, 6, false}
     {"switch_5",     7, 4, false}
     {"switch_6",     7, 6, false}
     {"switch_8",     7, 2, false}
   SWITCH_struct:
     {"AirDry",        11, 6, false, false}
     {"Hi-Temp Wash",  11, 4, false, false}
     {"QuickClean=",   12, 6, false, false}
     {"Normal",        13, 5, false, false}
     {"HeavyDuty",     13, 4, false, false}
*/

// ====== Internals ======
static TaskHandle_t s_matrix_task = NULL;
static SemaphoreHandle_t s_lock; // protects LEDS[] and SWITCHES[] updates

// Derived unique lists of rows/cols we actually touch
#define MAX_UNIQUE 16
static uint8_t s_led_rows[MAX_UNIQUE], s_led_row_count = 0;
static uint8_t s_led_cols[MAX_UNIQUE], s_led_col_count = 0;
static uint8_t s_sw_rows [MAX_UNIQUE], s_sw_row_count  = 0;
static uint8_t s_sw_cols [MAX_UNIQUE], s_sw_col_count  = 0;

// Debounce state
static uint8_t s_sw_cnt[sizeof(SWITCHES)/sizeof(SWITCHES[0])] = {0};
static bool    s_sw_stable[sizeof(SWITCHES)/sizeof(SWITCHES[0])] = {0};

// ====== Helpers ======
static void add_unique(uint8_t *arr, uint8_t *cnt, uint8_t v) {
  for (uint8_t i = 0; i < *cnt; ++i) if (arr[i] == v) return;
  if (*cnt < MAX_UNIQUE) arr[(*cnt)++] = v;
}

static void derive_sets(void) {
  s_led_row_count = s_led_col_count = s_sw_row_count = s_sw_col_count = 0;
  for (size_t i = 0; i < LED_COUNT; ++i) {
    add_unique(s_led_rows, &s_led_row_count, LEDS[i].row);
    add_unique(s_led_cols, &s_led_col_count, LEDS[i].col);
  }
  for (size_t i = 0; i < SWITCH_COUNT; ++i) {
    add_unique(s_sw_rows, &s_sw_row_count, SWITCHES[i].row);
    add_unique(s_sw_cols, &s_sw_col_count, SWITCHES[i].col);
  }
}

static void set_gpio_output(int gpio, int level) {
  if (gpio < 0) return;
  gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)gpio, level);
}

static void set_gpio_input_pullup(int gpio) {
  if (gpio < 0) return;
  gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_INPUT);
  // GPIO 34..39 do not have internal pull-ups/downs
  if (gpio >= 34 && gpio <= 39) {
    gpio_set_pull_mode((gpio_num_t)gpio, GPIO_FLOATING);
  } else {
    gpio_set_pull_mode((gpio_num_t)gpio, GPIO_PULLUP_ONLY);
  }
}

static int read_gpio(int gpio) {
  if (gpio < 0) return 1; // treat invalid as not pressed / idle
  return gpio_get_level((gpio_num_t)gpio);
}

static void cols_mode_output_led(const uint8_t *cols, uint8_t cnt, int idle_level) {
  for (uint8_t i = 0; i < cnt; ++i) {
    uint8_t w = cols[i];
    if (!wire_is_gpio(w)) continue;            // skip FIXED_GND/UNUSED
    int g = wire_gpio(w);
    gpio_set_direction((gpio_num_t)g, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)g, idle_level);
  }
}

static void cols_mode_input_pullup_sw(const uint8_t *cols, uint8_t cnt) {
  for (uint8_t i = 0; i < cnt; ++i) {
    uint8_t w = cols[i];
    if (!wire_is_gpio(w)) continue;            // columns that are FIXED_GND cannot be read
    set_gpio_input_pullup(wire_gpio(w));
  }
}

// Non-blocking "micro-wait": yields to scheduler instead of busy-waiting.
static inline void sleep_us_nonblocking(uint32_t us) {
  if (us < 1000) {
    taskYIELD(); // allow IDLE to feed the WDT
  } else {
    vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
  }
}

// ====== Validation ======
static esp_err_t validate_mapping(void) {
  bool ok = true;
  for (size_t i = 0; i < LED_COUNT; ++i) {
    if (!wire_is_gpio(LEDS[i].row)) {
      _LOG_E("LED '%s' row W%u not mapped to a GPIO.", LEDS[i].name, LEDS[i].row);
      ok = false;
    }
    if (!(wire_is_gpio(LEDS[i].col) || wire_is_fixed_gnd(LEDS[i].col))) {
      _LOG_E("LED '%s' col W%u not mapped (GPIO or FIXED_GND required).", LEDS[i].name, LEDS[i].col);
      ok = false;
    }
    if (wire_is_fixed_gnd(LEDS[i].col)) {
      _LOG_W("LED '%s' uses FIXED_GND column W%u: cannot be gated per-LED; lights when row W%u is active.",
             LEDS[i].name, LEDS[i].col, LEDS[i].row);
    }
  }
  for (size_t i = 0; i < SWITCH_COUNT; ++i) {
    if (!wire_is_gpio(SWITCHES[i].row)) {
      _LOG_E("SW '%s' row W%u not mapped to a GPIO.", SWITCHES[i].name, SWITCHES[i].row);
      ok = false;
    }
    if (!wire_is_gpio(SWITCHES[i].col)) {
      _LOG_E("SW '%s' col W%u not mapped to a GPIO (cannot read FIXED_GND as input).",
             SWITCHES[i].name, SWITCHES[i].col);
      ok = false;
    }
  }
  return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void preidle_all(void) {
  const int led_row_idle = !LED_ROW_ON_LEVEL;
  const int led_col_idle = !LED_COL_ON_LEVEL;
  const int sw_row_idle  = !SW_ROW_ACTIVE_LEVEL;

  for (uint8_t i = 0; i < s_led_row_count; ++i) set_gpio_output(wire_gpio(s_led_rows[i]), led_row_idle);

  // LED columns (only GPIO ones)
  cols_mode_output_led(s_led_cols, s_led_col_count, led_col_idle);

  // Switch rows/cols
  for (uint8_t i = 0; i < s_sw_row_count;  ++i) set_gpio_output(wire_gpio(s_sw_rows[i]),  sw_row_idle);
  cols_mode_input_pullup_sw(s_sw_cols, s_sw_col_count);
}

// ====== Matrix task ======
static void matrix_task(void *arg) {
  const TickType_t period_ticks = pdMS_TO_TICKS(1000 / MATRIX_SCAN_HZ);
  const TickType_t sec_ticks    = pdMS_TO_TICKS(SW_SCAN_PERIOD_MS);
  const TickType_t win_ticks    = pdMS_TO_TICKS(SW_SCAN_WINDOW_MS);

  const int led_row_idle = !LED_ROW_ON_LEVEL;
  const int led_col_idle = !LED_COL_ON_LEVEL;
  const uint8_t debounce_ticks = (DEBOUNCE_MS * MATRIX_SCAN_HZ) / 1000;
  if (debounce_ticks == 0) {
    _LOG_W("DEBOUNCE_MS too small for MATRIX_SCAN_HZ; effective 1 tick.");
  }

  preidle_all();

  TickType_t last_wake  = xTaskGetTickCount();
  TickType_t sec_anchor = last_wake;  // start of current 1s cycle

  while (1) {
    // Decide whether this frame should scan switches (duty-cycle window)
    TickType_t now = xTaskGetTickCount();
    TickType_t phase = now - sec_anchor;
    if (phase >= sec_ticks) {
      sec_anchor = now;
      phase = 0;
    }
    bool scan_switches_this_frame = (phase < win_ticks);

    // Clear Pressed_NOW at the start of the frame
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < SWITCH_COUNT; ++i) {
      SWITCHES[i].Pressed_NOW = false;
    }
    xSemaphoreGive(s_lock);

    /* ---- LED rows refresh ---- */
    for (uint8_t r = 0; r < s_led_row_count; ++r) {
      uint8_t row_wire = s_led_rows[r];
      int rg = wire_gpio(row_wire);
      if (rg < 0) continue;

      // Prepare LED columns (GPIO columns) as outputs idled
      cols_mode_output_led(s_led_cols, s_led_col_count, led_col_idle);

      // Activate this row
      set_gpio_output(rg, LED_ROW_ON_LEVEL);

      // Drive the columns for LEDs that are ON in this row
      xSemaphoreTake(s_lock, portMAX_DELAY);
      for (size_t i = 0; i < LED_COUNT; ++i) {
        if (LEDS[i].row != row_wire) continue;
        if (!LEDS[i].status) continue;

        uint8_t col_wire = LEDS[i].col;
        if (wire_is_gpio(col_wire)) {
          int cg = wire_gpio(col_wire);
          set_gpio_output(cg, LED_COL_ON_LEVEL);
        } // FIXED_GND will light automatically when row is active
      }
      xSemaphoreGive(s_lock);

      // Brief hold for visibility (scheduler-friendly)
      sleep_us_nonblocking(500);  // ~0.5 ms

      // Deactivate row and idle GPIO columns
      set_gpio_output(rg, led_row_idle);
      for (uint8_t i = 0; i < s_led_col_count; ++i) {
        uint8_t cwire = s_led_cols[i];
        if (!wire_is_gpio(cwire)) continue;
        set_gpio_output(wire_gpio(cwire), led_col_idle);
      }
    }

    /* ---- Switch rows scan (only within the active window) ---- */
    if (scan_switches_this_frame) {
      for (uint8_t r = 0; r < s_sw_row_count; ++r) {
        uint8_t row_wire = s_sw_rows[r];
        int rg = wire_gpio(row_wire);
        if (rg < 0) continue;

        // Prepare switch columns as inputs with pullups (where available)
        cols_mode_input_pullup_sw(s_sw_cols, s_sw_col_count);

        // Activate this row for scanning
        set_gpio_output(rg, SW_ROW_ACTIVE_LEVEL);
        sleep_us_nonblocking(50);  // settle (scheduler-friendly)

        // Sample each switch on this row
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < SWITCH_COUNT; ++i) {
          if (SWITCHES[i].row != row_wire) continue;
          int cg = wire_gpio(SWITCHES[i].col);
          int level = read_gpio(cg);
          bool pressed_sample = (level == SW_COL_PRESSED_LEVEL);

          // Debounce counter
          if (pressed_sample) {
            if (s_sw_cnt[i] < 255) s_sw_cnt[i]++;
          } else {
            if (s_sw_cnt[i] > 0) s_sw_cnt[i]--;
          }

          bool was_stable = s_sw_stable[i];
          bool new_stable = was_stable;

          if (!was_stable && s_sw_cnt[i] >= debounce_ticks) {
            new_stable = true; // just became pressed
            SWITCHES[i].Pressed_NOW = true;
            SWITCHES[i].Pressed_Registered = true;
          } else if (was_stable && s_sw_cnt[i] == 0) {
            new_stable = false; // released
          }
          s_sw_stable[i] = new_stable;
        }
        xSemaphoreGive(s_lock);

        // Deactivate row
        set_gpio_output(rg, !SW_ROW_ACTIVE_LEVEL);

        // If we ran out of window mid-scan, exit early
        now = xTaskGetTickCount();
        if ((now - sec_anchor) >= win_ticks) break;
      }
    }

    // Keep precise cadence; yields to scheduler (feeds WDT)
    vTaskDelayUntil(&last_wake, period_ticks);
  }
}

// ====== Name lookup ======
static int find_led_idx(const char *name) {
  for (size_t i = 0; i < LED_COUNT; ++i) if (strcmp(LEDS[i].name, name) == 0) return (int)i;
  return -1;
}
static int find_sw_idx(const char *name) {
  for (size_t i = 0; i < SWITCH_COUNT; ++i) if (strcmp(SWITCHES[i].name, name) == 0) return (int)i;
  return -1;
}

// ====== Public API: wire mapping ======
esp_err_t Matrix_BindWire(uint8_t wire, gpio_num_t gpio_num) {
  if (!wire_is_valid(wire)) return ESP_ERR_INVALID_ARG;
  s_wire_map[wire].kind = WIRE_GPIO;
  s_wire_map[wire].gpio = (int)gpio_num;
  return ESP_OK;
}

esp_err_t Matrix_BindWireFixedGND(uint8_t wire) {
  if (!wire_is_valid(wire)) return ESP_ERR_INVALID_ARG;
  s_wire_map[wire].kind = WIRE_FIXED_GND;
  s_wire_map[wire].gpio = -1;
  return ESP_OK;
}

// ====== Task start ======
static void start_matrix_task_once(void) {
  if (s_matrix_task) return;
  // Pin to core 1 to leave IDLE0 freer for WDT (optional but helpful)
  xTaskCreatePinnedToCore(matrix_task, "panel_matrix", MATRIX_TASK_STACK, NULL,
                          MATRIX_TASK_PRIO, &s_matrix_task, 1 /* core 1 */);
}

// ====== Public API: initialization ======
esp_err_t _init_LED(void) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  derive_sets();
  esp_err_t r = validate_mapping();
  if (r != ESP_OK) return r;
  preidle_all();
  start_matrix_task_once();
  _LOG_I("LED init: rows=%u cols=%u", s_led_row_count, s_led_col_count);
  return ESP_OK;
}

esp_err_t _init_Switch(void) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  derive_sets();
  esp_err_t r = validate_mapping();
  if (r != ESP_OK) return r;
  preidle_all();
  start_matrix_task_once();
  _LOG_I("SW init: rows=%u cols=%u", s_sw_row_count, s_sw_col_count);
  return ESP_OK;
}

// ====== Public API: LEDs ======
esp_err_t LED_Toggle(const char *name, led_cmd_t op) {
  int idx = find_led_idx(name);
  if (idx < 0) return ESP_ERR_NOT_FOUND;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool prev = LEDS[idx].status;
  if (op == LED_TOGGLE)       LEDS[idx].status = !prev;
  else if (op == LED_ON)      LEDS[idx].status = true;
  else /* LED_OFF */          LEDS[idx].status = false;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

bool LED_Get(const char *name) {
  int idx = find_led_idx(name);
  if (idx < 0) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool v = LEDS[idx].status;
  xSemaphoreGive(s_lock);
  return v;
}

// ====== Public API: Switches ======
void Switch_ClearRegistered(const char *name) {
  int idx = find_sw_idx(name);
  if (idx < 0) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  SWITCHES[idx].Pressed_Registered = false;
  xSemaphoreGive(s_lock);
}

bool Switch_Consume(const char *name) {
  int idx = find_sw_idx(name);
  if (idx < 0) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool was = SWITCHES[idx].Pressed_Registered;
  SWITCHES[idx].Pressed_Registered = false;
  xSemaphoreGive(s_lock);
  return was;
}

bool Switch_IsHeld(const char *name) {
  int idx = find_sw_idx(name);
  if (idx < 0) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool v = s_sw_stable[idx];
  xSemaphoreGive(s_lock);
  return v;
}

bool Switch_PressedNow(const char *name) {
  int idx = find_sw_idx(name);
  if (idx < 0) return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool v = SWITCHES[idx].Pressed_NOW;
  xSemaphoreGive(s_lock);
  return v;
}

// ====== Default harness mapping ======
// From your comment:
// Harness wires used: W1,W2,W3,W4,W5,W8,W9,W10,W12
//  - status_washing : A=W10 (GPIO17), C=W1 (GND)
//  - status_sensing : A=W9  (GPIO18), C=W3 (GPIO16)
//  - status_drying  : A=W10 (GPIO17), C=W4 (GPIO4)
//  - status_clean   : A=W8  (GPIO19), C=W5 (GPIO5)
//  - Start : column=W12, return=W4 (GPIO4)
//  - Cancel: column=W12, return=W2 (GPIO35)
void Panel_BindDefaultGPIOMap(void) {
  // W1 is physically GND on the harness
  Matrix_BindWireFixedGND(1);

  // Harness → ESP32 GPIO
  Matrix_BindWire(2,  GPIO_NUM_35);  // Cancel return (input-only; add ext PU)
  Matrix_BindWire(3,  GPIO_NUM_16);  // status_sensing column
  Matrix_BindWire(4,  GPIO_NUM_4);   // status_drying column + Start return
  Matrix_BindWire(5,  GPIO_NUM_5);   // status_clean column
  Matrix_BindWire(8,  GPIO_NUM_19);  // status_clean anode row
  Matrix_BindWire(9,  GPIO_NUM_18);  // status_sensing anode row
  Matrix_BindWire(10, GPIO_NUM_17);  // status_washing/status_drying anode row
  Matrix_BindWire(12, GPIO_NUM_23);  // shared switch column (Start/Cancel)
}
