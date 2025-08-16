
#include "dishwasher_programs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>


// Provided elsewhere; must be kept up-to-date (°F)
extern volatile int CurrentTemp;

#ifndef _LOG_D
#define _LOG_D(...) _LOG_D("DW_RUN", __VA_ARGS__)
#endif
#ifndef _LOG_I
#define _LOG_I(...) _LOG_I("DW_RUN", __VA_ARGS__)
#endif
#ifndef _LOG_W
#define _LOG_W(...) _LOG_W("DW_RUN", __VA_ARGS__)
#endif
#ifndef _LOG_E
#define _LOG_E(...) _LOG_E("DW_RUN", __VA_ARGS__)
#endif

// Weak HW glue so this compiles even if you haven't wired outputs yet.
// Replace/override these with your actual GPIO control.
__attribute__((weak)) void dw_set_mask(uint32_t mask)  { (void)mask; }
__attribute__((weak)) void dw_clear_mask(uint32_t mask){ (void)mask; }

// Helper: keep all non-heater bits of 'mask' set; optionally control heater bit.
static void apply_step_outputs(uint32_t mask, bool heater_on) {
    uint32_t non_heater = mask & ~DW_GPIO_HEAT;
    // Ensure non-heater actuators follow the mask for the whole step
    dw_set_mask(non_heater);
    // Heater follows heater_on flag only if step mask included HEAT
    if (mask & DW_GPIO_HEAT) {
        if (heater_on) dw_set_mask(DW_GPIO_HEAT);
        else           dw_clear_mask(DW_GPIO_HEAT);
    }
}

bool run_program_by_name(const char *program_name) {
    _LOG_D("Start of function");
    const Programs *prog = find_program_by_name(program_name);
    if (!prog) {
        _LOG_E("Program '%s' not found", program_name ? program_name : "(null)");
        _LOG_D("Exiting function");
        return false;
    }

    _LOG_I("Selected program: %s (steps=%u)", prog->name, (unsigned)prog->line_count);

    // Initial start delay: 30 seconds
    _LOG_I("Delaying start by 30 seconds...");
    vTaskDelay(pdMS_TO_TICKS(30 * 1000));

    for (size_t i = 0; i < prog->line_count; ++i) {
        const ProgramLine *ln = &prog->lines[i];
        const uint32_t mask = ln->gpio_mask;
        const int min_time_s = (int)ln->min_time;      // seconds
        const int max_temp_f = (int)ln->max_temp_f;    // degrees F (0 => ignore)
        const TickType_t poll_ticks = pdMS_TO_TICKS(30 * 1000);

        _LOG_I("Step %u/%u: cycle='%s' step='%s' min_time=%ds max_temp=%dF mask=0x%08x",
               (unsigned)(i+1), (unsigned)prog->line_count,
               ln->cycle ? ln->cycle : "", ln->step ? ln->step : "",
               min_time_s, max_temp_f, (unsigned)mask);

        // Start step: turn on everything in the mask; heater managed below.
        bool heater_on = (mask & DW_GPIO_HEAT) != 0;
        apply_step_outputs(mask, heater_on);

        TickType_t step_start = xTaskGetTickCount();
        TickType_t elapsed     = 0;

        bool reached_temp = false;
        TickType_t hold_until = 0;

        // If no max_temp requirement, just hold for min_time and move on.
        if (max_temp_f <= 0) {
            _LOG_I("No max_temp target; running for min_time=%ds", min_time_s);
            vTaskDelay(pdMS_TO_TICKS((min_time_s > 0 ? min_time_s : 0) * 1000));
            // Turn off all outputs for this step before moving on
            dw_clear_mask(mask);
            continue;
        }

        // Loop: poll every 30s
        for (;;) {
            vTaskDelay(poll_ticks);
            elapsed = xTaskGetTickCount() - step_start;

            int temp = CurrentTemp;  // snapshot
            _LOG_D("Temp poll: CurrentTemp=%dF, heater=%s", temp, heater_on ? "ON" : "OFF");

            if (!reached_temp && temp >= max_temp_f) {
                reached_temp = true;
                // Turn heater OFF immediately and start the hold
                if (heater_on) {
                    heater_on = false;
                    apply_step_outputs(mask, heater_on);
                    _LOG_I("Max temp reached (%dF). Heater OFF. Starting hold...", temp);
                }
                // Hold for max(15 minutes, min_time)
                int hold_s = min_time_s;
                if (hold_s < (15 * 60)) hold_s = 15 * 60;
                hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(hold_s * 1000);
            }

            if (reached_temp) {
                // Continue holding until hold_until
                if (xTaskGetTickCount() >= hold_until) {
                    _LOG_I("Hold complete (>= max(15min, min_time)). Ending step.");
                    break;
                }
                // Keep non-heater outputs on during the hold
                apply_step_outputs(mask, false);
                continue;
            }

            // Not yet reached max temp. If we've satisfied min_time, move on with a warning.
            if (min_time_s > 0 && elapsed >= pdMS_TO_TICKS(min_time_s * 1000)) {
                _LOG_W("Max temp not reached within min_time=%ds (CurrentTemp=%dF < %dF). Continuing to next step.",
                       min_time_s, temp, max_temp_f);
                break;
            }

            // Otherwise, keep waiting (heater stays on if included in mask).
            apply_step_outputs(mask, heater_on);
        }

        // End of this step: turn off everything that was on for this step
        dw_clear_mask(mask);
    }

    _LOG_I("Program '%s' complete.", prog->name);
    _LOG_D("Exiting function");
    return true;
}
