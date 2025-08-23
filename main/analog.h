#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/adc.h"  // For adc_channel_t enums etc.

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Defaults (can be overridden via analog_init) ===== */
#define ANALOG_DEFAULT_ADC_UNIT          ADC_UNIT_1
#define ANALOG_DEFAULT_ADC_CHANNEL       ADC_CHANNEL_6      /* GPIO34 on ADC1 */
#define ANALOG_DEFAULT_ADC_ATTEN         ADC_ATTEN_DB_11    /* ~ up to 3.3V */
#define ANALOG_DEFAULT_SAMPLE_PERIOD_MS  1000               /* 1 Hz */
#define ANALOG_DEFAULT_LOG_PERIOD_SEC    30
#define ANALOG_DEFAULT_WINDOW_SEC        60

/* Divider topology
 * Vs -> [Known R] ----o---- [Unknown R] -> GND
 *                      |
 *                     ADC
 * If your known resistor is at the bottom to GND, set KNOWN_RESISTOR_TOP to 0.
 */
#define KNOWN_RESISTOR_TOP               1

typedef struct {
  adc_unit_t   unit;                  /* ADC_UNIT_1 or ADC_UNIT_2 (oneshot supports both) */
  adc_channel_t channel;              /* e.g., ADC_CHANNEL_6 (GPIO34 on ADC1) */
  adc_atten_t  atten;                 /* e.g., ADC_ATTEN_DB_11 */
  float        source_voltage_v;      /* Vs: e.g., 5.0f */
  float        known_resistance_ohm;  /* Rk: e.g., 19700.0f */
  uint32_t     sample_period_ms;      /* e.g., 1000 */
  uint32_t     log_period_sec;        /* e.g., 30 */
  uint32_t     window_sec;            /* e.g., 60 */
} analog_config_t;

/* Initialize (creates internal buffers, configures ADC, no task started). */
bool analog_init(const analog_config_t *cfg);

/* Convenience init for Vs=5.0V, Rk=19.7k, 1 Hz, 60 s window, 30 s logging. */
bool analog_init_defaults(void);

/* Start/stop the sampler task (idempotent start; non-blocking loop). */
bool analog_start(void);
void analog_stop(void);

/* Data getters (thread-safe). */
float analog_get_latest_voltage_v(void);
float analog_get_avg_voltage_v(void);
float analog_get_latest_unknown_r_ohm(void);
float analog_get_avg_unknown_r_ohm(void);
uint32_t analog_get_window_size(void);
uint32_t analog_get_samples_collected(void);

#ifdef __cplusplus
}
#endif
