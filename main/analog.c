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
#include "dishwasher_programs.h"   /* provides _LOG_I(...) */

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
    _LOG_I("adc_oneshot_new_unit failed");
    return false;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {
    .atten    = cfg->atten,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_oneshot_config_channel(g.adc_handle, cfg->channel, &chan_cfg) != ESP_OK) {
    _LOG_I("adc_oneshot_config_channel failed");
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
    _LOG_I("ADC calibration unavailable; using raw scaling");
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

/* Weighted push, newest has weight n (or W when full) *
