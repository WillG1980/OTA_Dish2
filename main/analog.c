#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_err.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "analog.h"
#include "dishwasher_programs.h"   /* _LOG_I/_LOG_D/_LOG_W/_LOG_E */

/* ===== Internal state ===== */
typedef struct {
  analog_config_t cfg;

  /* ADC one-shot + calibration */
  adc_oneshot_unit_handle_t adc_handle;
  adc_cali_handle_t         cali_handle;
  bool                      cali_enabled;

  /* Ring buffers for last window_sec seconds */
  float    *voltage_v;        /* size: capacity */
  float    *unknown_r_ohm;    /* size: capacity */
  uint32_t  capacity;         /* == window_sec */
  uint32_t  head;             /* next write index */
  uint32_t  count;            /* <= capacity */

  /* Rolling sums */
  double    sum_v;            /* unweighted sum of V */
  double    sum_r;            /* unweighted sum of R */
  double    wsum_v;           /* weighted sum of V (weights 1..count, newest=count) */
  double    wsum_r;           /* weighted sum of R */

  /* Tasking */
  TaskHandle_t      task;
  SemaphoreHandle_t mtx;
  bool              running;
  uint32_t          seconds_since_log;
} analog_ctx_t;

static analog_ctx_t g;

/* ===== Helpers ===== */

static bool adc_setup_(const analog_config_t *cfg) {
  adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = cfg->unit };
  if (adc_oneshot_new_unit(&unit_cfg, &g.adc_handle) != ESP_OK) {
    _LOG_E("ANALOG", "adc_oneshot_new_unit failed");
    return false;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {
    .atten    = cfg->atten,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_oneshot_config_channel(g.adc_handle, cfg->channel, &chan_cfg) != ESP_OK) {
    _LOG_E("ANALOG", "adc_oneshot_config_channel failed");
    return false;
  }

  g.cali_enabled = false;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  {
    adc_cali_curve_fitting_config_t cal_cfg = {
      .unit_id  = cfg->unit,
      .atten    = cfg->atten,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &g.cali_handle) == ESP_OK) {
      g.cali_enabled = true;
      _LOG_I("ADC calibration: curve fitting enabled");
    }
  }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  if (!g.cali_enabled) {
    adc_cali_line_fitting_config_t cal_cfg = {
      .unit_id      = cfg->unit,
      .atten        = cfg->atten,
      .bitwidth     = ADC_BITWIDTH_DEFAULT,
      .default_vref = 1100,
    };
    if (adc_cali_create_scheme_line_fitting(&cal_cfg, &g.cali_handle) == ESP_OK) {
      g.cali_enabled = true;
      _LOG_I("ADC calibration: line fitting enabled");
    }
  }
#endif
  if (!g.cali_enabled) {
    _LOG_W("ANALOG", "ADC calibration unavailable; using raw scaling");
  }
  return true;
}

static float raw_to_voltage_v_(int raw) {
  int mv = 0;
  if (g.cali_enabled) {
    if (adc_cali_raw_to_voltage(g.cali_handle, raw, &mv) != ESP_OK) {
      mv = 0;
    }
  }
  if (!g.cali_enabled) {
    /* Rough fallback: 12-bit to 3.3V (atten affects real FS; this is approximate). */
    mv = (int)((raw * 3300) / 4095);
  }
  return (float)mv / 1000.0f;
}

/* Divider inference (see header for topology) */
static float infer_unknown_r_ohm_(float vout, float vs, float rk) {
  const float eps = 1e-6f;
#if KNOWN_RESISTOR_TOP
  float denom = (vs - vout);
  if (denom < eps) return INFINITY;
  return (vout * rk) / denom;
#else
  if (vout < eps) return INFINITY;
  return ((vs * rk) / vout) - rk;
#endif
}

static void ring_reset_(void) {
  if (g.voltage_v)     memset(g.voltage_v, 0, sizeof(float) * g.capacity);
  if (g.unknown_r_ohm) memset(g.unknown_r_ohm, 0, sizeof(float) * g.capacity);
  g.head = 0;
  g.count = 0;
  g.sum_v = g.sum_r = 0.0;
  g.wsum_v = g.wsum_r = 0.0;
  g.seconds_since_log = 0;
}

/* Update rolling sums with linear recency weights.
   Let n = current count (<= capacity).
   Weights are 1..n with newest weight n.
   On push x_new:
     if n < W:
       U' = U + x_new
       S_w' = S_w + U + (n+1)*x_new
       n' = n + 1
     if n == W:
       remove oldest x_old (weight 1)
       shift others -> S_w_temp = S_w - U
       U' = U - x_old + x_new
       S_w' = S_w_temp + W*x_new = (S_w - U) + W*x_new
*/
static void ring_push_weighted_(float v, float r) {
  const uint32_t W = g.capacity;

  if (g.count < W) {
    /* Append new at head */
    g.voltage_v[g.head]     = v;
    g.unknown_r_ohm[g.head] = r;
    /* weighted sums */
    g.wsum_v += g.sum_v + (double)(g.count + 1) * v;
    g.wsum_r += g.sum_r + (double)(g.count + 1) * r;
    /* unweighted sums */
    g.sum_v  += v;
    g.sum_r  += r;

    g.head = (g.head + 1) % W;
    g.count++;
  } else {
    /* Overwrite oldest (which is at head when full) */
    const uint32_t idx = g.head;
    const float v_old = g.voltage_v[idx];
    const float r_old = g.unknown_r_ohm[idx];

    /* Shift weights down by 1 for existing samples: S_w := S_w - U */
    g.wsum_v -= g.sum_v;
    g.wsum_r -= g.sum_r;

    /* Replace oldest with new sample */
    g.voltage_v[idx]     = v;
    g.unknown_r_ohm[idx] = r;

    /* Update unweighted sums */
    g.sum_v += v - v_old;
    g.sum_r += r - r_old;

    /* Add newest with weight W */
    g.wsum_v += (double)W * v;
    g.wsum_r += (double)W * r;

    g.head = (g.head + 1) % W;
    /* g.count stays == W */
  }
}

static inline double weight_denominator_(uint32_t n) {
  /* sum_{i=1..n} i = n*(n+1)/2 */
  return (double)n * (double)(n + 1) / 2.0;
}

/* ===== Sampler task ===== */

static void analog_task_(void *arg) {
  const TickType_t period = pdMS_TO_TICKS(g.cfg.sample_period_ms);
  TickType_t last_wake = xTaskGetTickCount();

  /* Align to cadence without blocking others */
  vTaskDelayUntil(&last_wake, period);

  while (g.running) {
    int raw = 0;
    if (adc_oneshot_read(g.adc_handle, g.cfg.channel, &raw) != ESP_OK) {
      _LOG_E("ANALOG", "adc_oneshot_read failed");
      raw = 0;
    }

    const float vout = raw_to_voltage_v_(raw);
    const float rx   = infer_unknown_r_ohm_(vout, g.cfg.source_voltage_v, g.cfg.known_resistance_ohm);

    xSemaphoreTake(g.mtx, portMAX_DELAY);
    ring_push_weighted_(vout, rx);

    const uint32_t n = g.count;
    const double denom = n ? weight_denominator_(n) : 1.0;
    const double wavg_v = n ? (g.wsum_v / denom) : 0.0;
    const double wavg_r = n ? (g.wsum_r / denom) : 0.0;

    g.seconds_since_log += g.cfg.sample_period_ms / 1000;
    const uint32_t secs = g.seconds_since_log;
    xSemaphoreGive(g.mtx);

    if (secs >= g.cfg.log_period_sec) {
      xSemaphoreTake(g.mtx, portMAX_DELAY);
      g.seconds_since_log = 0;
      xSemaphoreGive(g.mtx);

      _LOG_D("ANALOG",
             "raw=%d Vout=%.3fV Rx=%.1fΩ | wavg(%lus): V=%.3fV R=%.1fΩ | n=%lu",
             raw, vout, rx,
             (unsigned long)g.cfg.window_sec,
             (float)wavg_v, (float)wavg_r, (unsigned long)n);
    }

    vTaskDelayUntil(&last_wake, period);
  }

  _LOG_I("analog_task exiting");
  vTaskDelete(NULL);
}

/* ===== Public API ===== */

bool analog_init(const analog_config_t *cfg) {
  memset(&g, 0, sizeof(g));

  g.cfg.unit                 = cfg ? cfg->unit                : ANALOG_DEFAULT_ADC_UNIT;
  g.cfg.channel              = cfg ? cfg->channel             : ANALOG_DEFAULT_ADC_CHANNEL;
  g.cfg.atten                = cfg ? cfg->atten               : ANALOG_DEFAULT_ADC_ATTEN;
  g.cfg.source_voltage_v     = (cfg && cfg->source_voltage_v > 0)     ? cfg->source_voltage_v     : 5.0f;
  g.cfg.known_resistance_ohm = (cfg && cfg->known_resistance_ohm > 0) ? cfg->known_resistance_ohm : 19700.0f;
  g.cfg.sample_period_ms     = (cfg && cfg->sample_period_ms > 0)     ? cfg->sample_period_ms     : ANALOG_DEFAULT_SAMPLE_PERIOD_MS;
  g.cfg.log_period_sec       = (cfg && cfg->log_period_sec > 0)       ? cfg->log_period_sec       : ANALOG_DEFAULT_LOG_PERIOD_SEC;
  g.cfg.window_sec           = (cfg && cfg->window_sec > 0)           ? cfg->window_sec           : ANALOG_DEFAULT_WINDOW_SEC;

  g.capacity = g.cfg.window_sec;

  g.voltage_v     = (float*)heap_caps_malloc(sizeof(float) * g.capacity, MALLOC_CAP_DEFAULT);
  g.unknown_r_ohm = (float*)heap_caps_malloc(sizeof(float) * g.capacity, MALLOC_CAP_DEFAULT);
  if (!g.voltage_v || !g.unknown_r_ohm) {
    _LOG_E("ANALOG", "malloc failed for ring buffers");
    return false;
  }
  ring_reset_();

  g.mtx = xSemaphoreCreateMutex();
  if (!g.mtx) {
    _LOG_E("ANALOG", "mutex create failed");
    return false;
  }

  if (!adc_setup_(&g.cfg)) {
    return false;
  }

  _LOG_I("init: unit=%d ch=%d atten=%d Vs=%.2fV Rk=%.1fΩ period=%lums log=%lus window=%lus (weighted avg)",
         (int)g.cfg.unit, (int)g.cfg.channel, (int)g.cfg.atten,
         g.cfg.source_voltage_v, g.cfg.known_resistance_ohm,
         (unsigned long)g.cfg.sample_period_ms,
         (unsigned long)g.cfg.log_period_sec,
         (unsigned long)g.cfg.window_sec);
  return true;
}

bool analog_init_defaults(void) {
  analog_config_t cfg = {
    .unit = ANALOG_DEFAULT_ADC_UNIT,
    .channel = ANALOG_DEFAULT_ADC_CHANNEL,
    .atten = ANALOG_DEFAULT_ADC_ATTEN,
    .source_voltage_v = 5.0f,
    .known_resistance_ohm = 19700.0f,
    .sample_period_ms = 1000,
    .log_period_sec = 30,
    .window_sec = 60,
  };
  return analog_init(&cfg);
}

bool analog_start(void) {
  if (g.running) {
    _LOG_D("ANALOG", "analog_start: already running");
    return true;
  }
  g.running = true;
  const BaseType_t ok = xTaskCreate(analog_task_, "analog_task", 4096, NULL, 5, &g.task);
  if (ok != pdPASS) {
    g.running = false;
    _LOG_E("ANALOG", "failed to create analog_task");
    return false;
  }
  _LOG_I("analog_task started");
  return true;
}

void analog_stop(void) {
  if (!g.running) return;
  g.running = false;
  vTaskDelay(pdMS_TO_TICKS(5));

  if (g.adc_handle) {
    adc_oneshot_del_unit(g.adc_handle);
    g.adc_handle = NULL;
  }
  if (g.cali_enabled && g.cali_handle) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(g.cali_handle);
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(g.cali_handle);
#endif
    g.cali_handle = NULL;
    g.cali_enabled = false;
  }
}

/* ===== Getters (weighted averages) ===== */

float analog_get_latest_voltage_v(void) {
  float v = 0.0f;
  xSemaphoreTake(g.mtx, portMAX_DELAY);
  if (g.count) {
    const uint32_t last = (g.head + g.capacity - 1) % g.capacity;
    v = g.voltage_v[last];
  }
  xSemaphoreGive(g.mtx);
  return v;
}

float analog_get_weighted_avg_voltage_v(void) {
  double out = 0.0;
  xSemaphoreTake(g.mtx, portMAX_DELAY);
  if (g.count) out = g.wsum_v / weight_denominator_(g.count);
  xSemaphoreGive(g.mtx);
  return (float)out;
}

float analog_get_latest_unknown_r_ohm(void) {
  float r = 0.0f;
  xSemaphoreTake(g.mtx, portMAX_DELAY);
  if (g.count) {
    const uint32_t last = (g.head + g.capacity - 1) % g.capacity;
    r = g.unknown_r_ohm[last];
  }
  xSemaphoreGive(g.mtx);
  return r;
}

float analog_get_weighted_avg_unknown_r_ohm(void) {
  double out = 0.0;
  xSemaphoreTake(g.mtx, portMAX_DELAY);
  if (g.count) out = g.wsum_r / weight_denominator_(g.count);
  xSemaphoreGive(g.mtx);
  return (float)out;
}

uint32_t analog_get_window_size(void) {
  return g.capacity;
}

uint32_t analog_get_samples_collected(void) {
  uint32_t n = 0;
  xSemaphoreTake(g.mtx, portMAX_DELAY);
  n = g.count;
  xSemaphoreGive(g.mtx);
  return n;
}

/* ===== One-call bootstrap for app_main() ===== */

bool _start_temp_monitor(void) {
  if (!analog_init_defaults()) {
    _LOG_E("ANALOG", "_start_temp_monitor: init failed");
    return false;
  }
  if (!analog_start()) {
    _LOG_E("ANALOG", "_start_temp_monitor: start failed");
    return false;
  }
  _LOG_I("ANALOG Operature monitor started (1 Hz, weighted 60s avg, log every 30s)");
  return true;
}
