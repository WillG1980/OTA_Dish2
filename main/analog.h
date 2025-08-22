// analog_temp_monitor.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the GPIO34 (ADC1_CH6) temperature monitor.
 * - Samples raw ADC at 10 Hz.
 * - Maintains a 60 s rolling, recency-weighted average.
 * - Logs every 30 s: _LOG_I(TAG, "Current ADC reading: %d", avg);
 *
 * Safe to call multiple times (subsequent calls are no-ops if already running).
 */
void _start_temp_monitor(void);

/**
 * Stop the temperature monitor task (if running).
 * The task self-deletes; this call returns immediately.
 */
void _stop_temp_monitor(void);

#ifdef __cplusplus
}
#endif
