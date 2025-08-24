// analog.c — drop‑in replacement for your temp sampler (build-248-ok)
// Logs everything per sample: raw instant, raw mean (oversampled), min/max/stddev,
// EWMA (your weighted average), calibrated mV, computed Rth (Ω), and temperature.
// Keeps your public API: _start_temp_monitor() / _stop_temp_monitor().
// Preserves the legacy line: "Current ADC reading: <value>" (now uses EWMA).

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "dishwasher_programs.h"

// ──────────────────────────────────────────────────────────────────────────────
// Configuration
// ──────────────────────────────────────────────────────────────────────────────
#define ANALOG_ADC_UNIT          ADC_UNIT_1
#define ANALOG_ADC_CH            ADC_CHANNEL_6      // GPIO34 = ADC1_CH6
#define ANALOG_ADC_ATTEN         ADC_ATTEN_DB_11    // ~3.3V full-scale on ADC1
#define ANALOG_BITWIDTH          ADC_BITWIDTH_DEFAULT

#define SAMPLE_PERIOD_MS         100                // 10 Hz
#define OVERSAMPLE_N             16                 // readings per log tick
#define EWMA_ALPHA               0.10f              // weighted-average smoothing factor

// Divider / thermistor model
#define VSUPPLY_MV               3300.0f            // set to 5000 if running divider from 5V
#define R_KNOWN_OHMS             19700.0f           // your fixed resistor value
#define THERM_ON_TOP             true               // true: Thermistor→Vs, Rk→GND; false if flipped

// Beta model (adjust to your part if known)
#define BETA                     3950.0f
#define R25_OHMS                 10000.0f           // 10k @ 25°C typical

// ──────────────────────────────────────────────────────────────────────────────
// State
// ──────────────────────────────────────────────────────────────────────────────
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t         s_cali = NULL;
static bool  s_cal_ok = false;
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static float s_ewma = NAN;                          // your weighted average

// ──────────────────────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────────────────────
static inline uint32_t now_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline int raw_to_mv(int raw) {
  if (s_cal_ok) {
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
  }
  // Rough fallback mapping for 11 dB
  return (int)((raw / 4095.0f) * 3300.0f);
}

static inline float compute_rth_ohms(float vnode_mv) {
  const float vs = VSUPPLY_MV;
  if (THERM_ON_TOP) {
    // V = Vs * Rk/(Rk+Rth) ⇒ Rth = Rk*(Vs/V − 1)
    if (vnode_mv < 1.0f) vnode_mv = 1.0f;
    return R_KNOWN_OHMS * (vs / vnode_mv - 1.0f);
  } else {
    // V = Vs * Rth/(Rk+Rth) ⇒ Rth = Rk * V/(Vs − V)
    if ((vs - vnode_mv) < 1.0f) vnode_mv = vs - 1.0f;
    return R_KNOWN_OHMS * (vnode_mv / (vs - vnode_mv));
  }
}

static inline float tempC_from_beta(float Rth) {
  if (Rth <= 0.0f || BETA <= 0.0f || R25_OHMS <= 0.0f) return NAN;
  const float T0 = 298.15f; // 25°C in K
  float invT = (1.0f/T0) + (1.0f/BETA) * logf(Rth / R25_OHMS);
  return (1.0f/invT) - 273.15f;
}

// ──────────────────────────────────────────────────────────────────────────────
// ADC init
// ──────────────────────────────────────────────────────────────────────────────
static esp_err_t init_adc_oneshot(void) {
  if (s_adc) return ESP_OK;

  adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ANALOG_ADC_UNIT };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

  adc_oneshot_chan_cfg_t ch_cfg = {
    .bitwidth = ANALOG_BITWIDTH,
    .atten    = ANALOG_ADC_ATTEN,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ANALOG_ADC_CH, &ch_cfg));

  // Calibration (curve-fitting)
  adc_cali_curve_fitting_config_t cal_cfg = {
    .unit_id = ANALOG_ADC_UNIT,
    .chan    = ANALOG_ADC_CH,
    .atten   = ANALOG_ADC_ATTEN,
    .bitwidth = ANALOG_BITWIDTH,
  };
  if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali) == ESP_OK) {
    s_cal_ok = true;
    _LOG_I("ADC calibration: curve-fitting enabled");
  } else {
    s_cal_ok = false;
    _LOG_W("ADC calibration unavailable; mv will be estimated");
  }

  _LOG_I("ADC oneshot set up on ADC1_CH6 (GPIO34)");
  return ESP_OK;
}

// ──────────────────────────────────────────────────────────────────────────────
// One full read cycle with oversampling and detailed logging
// ──────────────────────────────────────────────────────────────────────────────
static void log_full_sample(void) {
  // Oversample
  int min_raw = INT_MAX, max_raw = INT_MIN;
  int64_t sum_raw = 0;
  int last_raw = 0;
  for (int i = 0; i < OVERSAMPLE_N; ++i) {
    int r = 0;
    esp_err_t er = adc_oneshot_read(s_adc, ANALOG_ADC_CH, &r);
    if (er != ESP_OK) {
      _LOG_W("adc_oneshot_read error=%d", (int)er);
      continue;
    }
    last_raw = r;
    sum_raw += r;
    if (r < min_raw) min_raw = r;
    if (r > max_raw) max_raw = r;
  }
  const float mean_raw = (float)sum_raw / (float)OVERSAMPLE_N;

  // stddev (second pass not needed; use Welford if you prefer)
  double acc = 0.0;
  for (int i = 0; i < OVERSAMPLE_N; ++i) {
    // We didn’t store each sample, so approximate stddev from range (robust & cheap)
    // NOTE: for precise stddev, store the N samples and compute directly.
    // Here we estimate σ ≈ (max−min)/sqrt(12) assuming uniform noise.
  }
  const float std_est = (float)( (max_raw - min_raw) / sqrt(12.0) );

  // EWMA update
  if (isnan(s_ewma)) s_ewma = mean_raw; else s_ewma = (1.0f - EWMA_ALPHA)*s_ewma + EWMA_ALPHA*mean_raw;

  // Conversions based on mean
  const int   mv_mean = raw_to_mv((int)lroundf(mean_raw));
  const float rth     = compute_rth_ohms((float)mv_mean);
  const float tC      = tempC_from_beta(rth);
  const float tF      = isnan(tC) ? NAN : (tC * 9.0f/5.0f + 32.0f);

  // Also compute from instantaneous last_raw for reference
  const int mv_inst = raw_to_mv(last_raw);

  // Legacy line (kept exactly so any scrapers/scripts don’t break):
  _LOG_I("Current ADC reading: %d", (int)lroundf(s_ewma));

  // Detailed structured line (JSON-ish, one per sample cycle)
  _LOG_D(
    "ADC_SAMPLE {raw_inst:%d,mv_inst:%d,raw_mean:%d,mv_mean:%d,raw_min:%d,raw_max:%d,raw_std_est:%.1f,ewma:%.1f,atten_db:%d,bit:%d,vs_mv:%.0f,top:%d,Rk_ohm:%.0f,Rth_ohm:%.0f,tempC:%.2f,tempF:%.2f,os_n:%d}",
    last_raw, mv_inst, (int)lroundf(mean_raw), mv_mean, min_raw, max_raw, (double)std_est, (double)s_ewma,
    (int)ANALOG_ADC_ATTEN, (int)ANALOG_BITWIDTH, (double)VSUPPLY_MV, THERM_ON_TOP ? 1 : 0,
    (double)R_KNOWN_OHMS, (double)rth, (double)tC, (double)tF, OVERSAMPLE_N
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// Sampler task
// ──────────────────────────────────────────────────────────────────────────────
static void temp_sampler_task(void *arg) {
  (void)arg;
  if (init_adc_oneshot() != ESP_OK) {
    _LOG_E("ADC init failed; exiting sampler task");
    vTaskDelete(NULL);
    return;
  }
  s_running = true;

  uint32_t t_last = now_ms();
  while (s_running) {
    uint32_t t0 = now_ms();
    log_full_sample();

    // Keep exact sample cadence
    uint32_t elapsed = now_ms() - t0;
    uint32_t wait_ms = (elapsed >= SAMPLE_PERIOD_MS) ? 1 : (SAMPLE_PERIOD_MS - elapsed);
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    t_last = now_ms();
  }

  vTaskDelete(NULL);
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────────────────────
void _start_temp_monitor(void) {
  if (s_task) { _LOG_I("temp monitor already running"); return; }
  if (init_adc_oneshot() != ESP_OK) { _LOG_E("ADC init failed"); return; }
  BaseType_t ok = xTaskCreate(temp_sampler_task, "temp_sampler", 4096, NULL, 5, &s_task);
  if (ok != pdPASS) { s_task = NULL; _LOG_E("failed to create temp_sampler task"); }
}

void _stop_temp_monitor(void) {
  s_running = false;
  vTaskDelay(pdMS_TO_TICKS(20));
  s_task = NULL;
}