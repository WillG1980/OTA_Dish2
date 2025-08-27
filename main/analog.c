// analog.c — Option A drop‑in replacement (ESP-IDF 5.x)
// - Collects full sample stats every SAMPLE_PERIOD_MS
// - Logs only every _LOG_FREQ_ seconds
// - Disables temperature sampling/logging while no program is active
//   (ActiveStatus.Program must be non-empty to run)
// - Preserves legacy line: "Current ADC reading: <int>" at the chosen log
// cadence
// - Uses ADC calibration (line or curve fitting) when available
//
// Notes:
// • Set VSUPPLY_MV to 3300 or 5000 depending on your divider feed.
// • Set THERM_ON_TOP true if Thermistor→Vsupply, Rk→GND (counts rise with
// temp),
//   false if Thermistor→GND, Rk→Vsupply (counts fall with temp).

#include "dishwasher_programs.h" // for ActiveStatus.Program
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>
#include <string.h>


// ──────────────────────────────────────────────────────────────────────────────
// Configuration
// ──────────────────────────────────────────────────────────────────────────────
#define ANALOG_ADC_UNIT ADC_UNIT_1
#define ANALOG_ADC_CH ADC_CHANNEL_6      // GPIO34 = ADC1_CH6
#define ANALOG_ADC_ATTEN ADC_ATTEN_DB_11 // ~3.3V full-scale on ADC1
#define ANALOG_BITWIDTH ADC_BITWIDTH_DEFAULT

#define SAMPLE_PERIOD_MS 100 // collect at 10 Hz
#define OVERSAMPLE_N 16      // readings per collection
#define EWMA_ALPHA 0.10f     // weighted-average smoothing factor [0..1]
#define _LOG_FREQ_ 10        // seconds between log prints

// Divider / thermistor model
#define VSUPPLY_MV 3300.0f    // 3300 or 5000 depending on your wiring
#define R_KNOWN_OHMS 19700.0f // fixed resistor
#define THERM_ON_TOP true     // true: Thermistor→Vs, false: Thermistor→GND

// Beta model (tune to your part as needed)
#define BETA 3950.0f
#define R25_OHMS 10000.0f // 10k @ 25°C typical

// ──────────────────────────────────────────────────────────────────────────────
// State & types
// ──────────────────────────────────────────────────────────────────────────────
typedef struct SampleStats {
  int raw_inst; // last instantaneous reading in the window
  int raw_min;
  int raw_max;
  int raw_mean;  // integer mean of window
  float raw_std; // standard deviation of window

  int mv_inst; // calibrated mV (instant)
  int mv_mean; // calibrated mV (mean)

  float ewma; // filtered code (weighted average)

  float Rth_ohm; // computed thermistor resistance from divider
  float tempC;   // Beta model
  float tempF;
} SampleStats;

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cal_ok = false;
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static float s_ewma = NAN; // persisted EWMA across samples

// ──────────────────────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────────────────────
static inline uint32_t now_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static inline bool program_running(void) {
  // Treat non-empty Program string as "running"; adjust if you use a sentinel
  // like "None".
  return (ActiveStatus.Program[0] != '\0');
}

static inline int raw_to_mv(int raw) {
  if (s_cal_ok) {
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK)
      return mv;
  }
  // Fallback rough mapping for 11 dB on ADC1 (~0..3300 mV for 0..4095)
  return (int)((raw / 4095.0f) * 3300.0f);
}

static inline float compute_rth_ohms_from_mv(float vnode_mv) {
  const float vs = VSUPPLY_MV;
  if (THERM_ON_TOP) {
    // V = Vs * Rk/(Rk + Rth)  ⇒  Rth = Rk*(Vs/V − 1)
    if (vnode_mv < 1.0f)
      vnode_mv = 1.0f;
    return R_KNOWN_OHMS * (vs / vnode_mv - 1.0f);
  } else {
    // V = Vs * Rth/(Rk + Rth) ⇒  Rth = Rk * V/(Vs − V)
    if ((vs - vnode_mv) < 1.0f)
      vnode_mv = vs - 1.0f;
    return R_KNOWN_OHMS * (vnode_mv / (vs - vnode_mv));
  }
}

static inline float tempC_from_beta(float Rth) {
  if (Rth <= 0.0f || BETA <= 0.0f || R25_OHMS <= 0.0f)
    return NAN;
  const float T0 = 298.15f; // 25°C in K
  const float invT = (1.0f / T0) + (1.0f / BETA) * logf(Rth / R25_OHMS);
  return (1.0f / invT) - 273.15f;
}

// ──────────────────────────────────────────────────────────────────────────────
// ADC init (portable across ESP32 variants)
// ──────────────────────────────────────────────────────────────────────────────
static esp_err_t init_adc_oneshot(void) {
  if (s_adc)
    return ESP_OK;

  adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ANALOG_ADC_UNIT};
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

  adc_oneshot_chan_cfg_t ch_cfg = {
      .bitwidth = ANALOG_BITWIDTH,
      .atten = ANALOG_ADC_ATTEN,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ANALOG_ADC_CH, &ch_cfg));

  // Calibration (prefer line fitting where available; fallback to curve if
  // present)
  bool calibrated = false;
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  {
    adc_cali_line_fitting_config_t cal_cfg = {
        .unit_id = ANALOG_ADC_UNIT,
        .atten = ANALOG_ADC_ATTEN,
        .bitwidth = ANALOG_BITWIDTH,
    };
    if (adc_cali_create_scheme_line_fitting(&cal_cfg, &s_cali) == ESP_OK) {
      s_cal_ok = true;
      calibrated = true;
      _LOG_I("ADC calibration: line fitting enabled");
    }
  }
#endif
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  if (!calibrated) {
    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id = ANALOG_ADC_UNIT,
        .atten = ANALOG_ADC_ATTEN,
        .bitwidth = ANALOG_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali) == ESP_OK) {
      s_cal_ok = true;
      calibrated = true;
      _LOG_I("ADC calibration: curve fitting enabled");
    }
  }
#endif
  if (!calibrated) {
    s_cal_ok = false;
    _LOG_W("ADC calibration not supported; using raw->mV fallback");
  }

  _LOG_I("ADC oneshot set up on ADC1_CH6 (GPIO34)");
  return ESP_OK;
}

// ──────────────────────────────────────────────────────────────────────────────
// Collector: reads once, fills SampleStats; no logging here (Option A)
// ──────────────────────────────────────────────────────────────────────────────
static void collect_full_sample(SampleStats *out) {
  int buf[OVERSAMPLE_N];
  int min_raw = INT_MAX, max_raw = INT_MIN;
  int64_t sum = 0;

  for (int i = 0; i < OVERSAMPLE_N; ++i) {
    int r = 0;
    esp_err_t er = adc_oneshot_read(s_adc, ANALOG_ADC_CH, &r);
    if (er != ESP_OK) {
      _LOG_W("adc_oneshot_read error=%d", (int)er);
      r = 0;
    }
    buf[i] = r;
    if (r < min_raw)
      min_raw = r;
    if (r > max_raw)
      max_raw = r;
    sum += r;
  }

  const float mean_raw_f = (float)sum / (float)OVERSAMPLE_N;

  // Exact stddev
  double acc = 0.0;
  for (int i = 0; i < OVERSAMPLE_N; ++i) {
    const double d = (double)buf[i] - (double)mean_raw_f;
    acc += d * d;
  }
  const float std_raw = sqrtf((float)(acc / (double)OVERSAMPLE_N));

  // EWMA based on mean
  if (isnan(s_ewma))
    s_ewma = mean_raw_f;
  else
    s_ewma = (1.0f - EWMA_ALPHA) * s_ewma + EWMA_ALPHA * mean_raw_f;

  // Populate struct
  out->raw_inst = buf[OVERSAMPLE_N - 1];
  out->raw_min = min_raw;
  out->raw_max = max_raw;
  out->raw_mean = (int)lroundf(mean_raw_f);
  out->raw_std = std_raw;
  out->mv_inst = raw_to_mv(out->raw_inst);
  out->mv_mean = raw_to_mv(out->raw_mean);
  out->ewma = s_ewma;
  out->Rth_ohm = compute_rth_ohms_from_mv((float)out->mv_mean);
  out->tempC = tempC_from_beta(out->Rth_ohm);
  out->tempF = isnan(out->tempC) ? NAN : (out->tempC * 9.0f / 5.0f + 32.0f);
}

// ──────────────────────────────────────────────────────────────────────────────
// Sampler task: collects at fixed rate, logs only every _LOG_FREQ_ seconds
// ──────────────────────────────────────────────────────────────────────────────
static void temp_sampler_task(void *arg) {
  (void)arg;
  if (init_adc_oneshot() != ESP_OK) {
    _LOG_E("ADC init failed; exiting sampler task");
    vTaskDelete(NULL);
    return;
  }
  s_running = true;
  s_ewma = NAN;

  uint32_t last_log_ms = 0;

  while (s_running) {
    const uint32_t t_loop = now_ms();

    //if (!program_running()) {
            if (false) {
      // idle: do not collect or log; sleep a bit, clear EWMA so we don't carry
      // stale state
      if (!isnan(s_ewma))
        s_ewma = NAN;
      vTaskDelay(pdMS_TO_TICKS(250));
    } else {
      SampleStats st = {0};
      collect_full_sample(&st);

      // only print every _LOG_FREQ_ seconds
      const uint32_t now = now_ms();
      if ((now - last_log_ms) >= (_LOG_FREQ_ * 1000)) {
        last_log_ms = now;

        // Legacy line for scripts: Current ADC reading (EWMA)
        //_LOG_I("Current ADC reading: %d", (int)lroundf(st.ewma));

        // Rich structured line for detailed analysis
        _LOG_I("ADC_SAMPLE "
               "{raw_inst:%d,mv_inst:%d,raw_mean:%d,mv_mean:%d,raw_min:%d,raw_"
               "max:%d,raw_std:%.1f,ewma:%.1f,atten_db:%d,bit:%d,vs_mv:%.0f,"
               "top:%d,Rk_ohm:%.0f,Rth_ohm:%.0f,tempC:%.2f,tempF:%.2f,os_n:%d}",
               st.raw_inst, st.mv_inst, st.raw_mean, st.mv_mean, st.raw_min,
               st.raw_max, (double)st.raw_std, (double)st.ewma,
               (int)ANALOG_ADC_ATTEN, (int)ANALOG_BITWIDTH, (double)VSUPPLY_MV,
               THERM_ON_TOP ? 1 : 0, (double)R_KNOWN_OHMS, (double)st.Rth_ohm,
               (double)st.tempC, (double)st.tempF, OVERSAMPLE_N);

        float temp_f = 0.059031f * (float)st.mv_mean + 27.381f;
        ActiveStatus.CurrentTemp= (int)(temp_f + 0.5f); // round to nearest int

        _LOG_I("update_current_temp_from_adc(): mv_mean=%d → Temp=%d°F",
               st.mv_mean, ActiveStatus.CurrentTemp);
      }

      // pacing to maintain collection cadence
      const uint32_t elapsed = now_ms() - t_loop;
      const uint32_t wait_ms =
          (elapsed >= SAMPLE_PERIOD_MS) ? 1 : (SAMPLE_PERIOD_MS - elapsed);
      vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
  }

  vTaskDelete(NULL);
}

// ──────────────────────────────────────────────────────────────────────────────
// Public API expected by the rest of your project
// ──────────────────────────────────────────────────────────────────────────────
void _start_temp_monitor(void) {
  if (s_task) {
    _LOG_I("temp monitor already running");
    return;
  }
  if (init_adc_oneshot() != ESP_OK) {
    _LOG_E("ADC init failed");
    return;
  }
  BaseType_t ok =
      xTaskCreate(temp_sampler_task, "temp_sampler", 4096, NULL, 5, &s_task);
  if (ok != pdPASS) {
    s_task = NULL;
    _LOG_E("failed to create temp_sampler task");
  }
}

void _stop_temp_monitor(void) {
  s_running = false;
  vTaskDelay(pdMS_TO_TICKS(20));
  s_task = NULL;
}
