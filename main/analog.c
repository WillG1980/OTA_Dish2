// analog_temp_monitor.c (or merge into your analog.c)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include <string.h>
#include <math.h>
#include <stdint.h>

// ---- Your logging macro style ----
#ifndef TAG
#define TAG "ANALOG"
#endif
#ifndef _LOG_I
#define _LOG_I(TAG_, fmt, ...) ESP_LOGI(TAG_, fmt, ##__VA_ARGS__)
#endif
#ifndef _LOG_W
#define _LOG_W(TAG_, fmt, ...) ESP_LOGW(TAG_, fmt, ##__VA_ARGS__)
#endif
#ifndef _LOG_E
#define _LOG_E(TAG_, fmt, ...) ESP_LOGE(TAG_, fmt, ##__VA_ARGS__)
#endif

// ---- Configuration ----
#define TEMP_ADC_UNIT        ADC_UNIT_1
#define TEMP_ADC_CH          ADC_CHANNEL_6     // GPIO34 = ADC1_CH6
#define TEMP_ADC_ATTEN       ADC_ATTEN_DB_11   // up to ~3.3V
#define SAMPLE_PERIOD_MS     100               // 10 Hz
#define WINDOW_MS            60000             // 60 s rolling window
#define LOG_EVERY_MS         30000             // log every 30 s

// Buffer sizing: at 10 Hz for 60 s -> 600 samples
#define BUF_CAP              (WINDOW_MS / SAMPLE_PERIOD_MS + 8) // a bit of slack

typedef struct {
  uint32_t ts_ms;  // sample timestamp (ms since boot)
  int raw;         // raw ADC code
} sample_t;

static adc_oneshot_unit_handle_t s_adc = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static sample_t s_buf[BUF_CAP];
static int s_head = 0;
static int s_count = 0;

// ---- Time helpers ----
static inline uint32_t now_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// ---- Ring buffer push ----
static inline void push_sample(uint32_t ts_ms, int raw) {
  s_buf[s_head].ts_ms = ts_ms;
  s_buf[s_head].raw   = raw;
  s_head = (s_head + 1) % BUF_CAP;
  if (s_count < BUF_CAP) {
    s_count++;
  } else {
    // overwrite oldest; count stays at capacity
  }
}

// ---- Compute recency-weighted average over last WINDOW_MS ----
// Weight each sample linearly by recency within the window:
//   w = (sample_ts - (now - WINDOW_MS)) / WINDOW_MS   (clamped to 0..1)
static float weighted_avg_raw_last_window(uint32_t now) {
  if (s_count == 0) return NAN;

  const uint32_t window_start = (now > WINDOW_MS) ? (now - WINDOW_MS) : 0;
  double wsum = 0.0;
  double xsum = 0.0;

  // iterate newest->oldest up to buffer size
  for (int i = 0; i < s_count; ++i) {
    int idx = (s_head - 1 - i);
    if (idx < 0) idx += BUF_CAP;
    const sample_t *s = &s_buf[idx];

    if (s->ts_ms < window_start) break; // older than window, done

    double w = (double)(s->ts_ms - window_start) / (double)WINDOW_MS;
    if (w < 0.0) w = 0.0;
    if (w > 1.0) w = 1.0;
    wsum += w;
    xsum += w * (double)s->raw;
  }

  if (wsum <= 0.0) return NAN;
  return (float)(xsum / wsum);
}

// ---- ADC init ----
static esp_err_t init_adc_oneshot(void) {
  if (s_adc) return ESP_OK;

  adc_oneshot_unit_init_cfg_t unit_cfg = {
    .unit_id = TEMP_ADC_UNIT,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

  adc_oneshot_chan_cfg_t ch_cfg = {
    .bitwidth = ADC_BITWIDTH_DEFAULT,
    .atten = TEMP_ADC_ATTEN,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, TEMP_ADC_CH, &ch_cfg));

  _LOG_I("ADC oneshot set up on ADC1_CH6 (GPIO34)");
  return ESP_OK;
}

// ---- Sampler task ----
static void temp_sampler_task(void *arg) {
  (void)arg;
  if (init_adc_oneshot() != ESP_OK) {
    _LOG_E(TAG, "ADC init failed; exiting sampler task");
    vTaskDelete(NULL);
    return;
  }
  s_running = true;

  uint32_t last_log = now_ms();

  while (s_running) {
    uint32_t t0 = now_ms();

    // Read raw ADC
    int raw = 0;
    esp_err_t er = adc_oneshot_read(s_adc, TEMP_ADC_CH, &raw);
    if (er != ESP_OK) {
      _LOG_W(TAG, "adc_oneshot_read error=%d", (int)er);
    } else {
      push_sample(t0, raw);
    }

    // Periodic logging
    uint32_t t_now = now_ms();
    if ((t_now - last_log) >= LOG_EVERY_MS) {
      float wav = weighted_avg_raw_last_window(t_now);
      if (!isnan(wav)) {
        // EXACT string required by you (with your macro style):
        _LOG_I("Current ADC reading: %d", (int)lroundf(wav));
      } else {
        _LOG_I("Current ADC reading: N/A");
      }
      last_log = t_now;
    }

    // Sleep until next sample
    uint32_t elapsed = now_ms() - t0;
    uint32_t wait_ms = (elapsed >= SAMPLE_PERIOD_MS) ? 1 : (SAMPLE_PERIOD_MS - elapsed);
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
  }

  vTaskDelete(NULL);
}

// ---- Public API ----
void _start_temp_monitor(void) {
  if (s_task) {
    _LOG_I("temp monitor already running");
    return;
  }
  if (init_adc_oneshot() != ESP_OK) {
    _LOG_E(TAG, "ADC init failed");
    return;
  }
  BaseType_t ok = xTaskCreate(
      temp_sampler_task, "temp_sampler", 3072, NULL, 5, &s_task);
  if (ok != pdPASS) {
    s_task = NULL;
    _LOG_E(TAG, "failed to create temp_sampler task");
  }
}

void _stop_temp_monitor(void) {
  s_running = false;
  // task self-deletes; give it a tick to exit and clear handle
  vTaskDelay(pdMS_TO_TICKS(20));
  s_task = NULL;
}

