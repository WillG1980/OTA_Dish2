#include "analog.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_log.h" // if you prefer ESP_LOGI instead of _LOG_I
#include "esp_netif.h"
#include "esp_timer.h" // esp_timer_get_time()
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "http_utils.h"
#include "nvs_flash.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include <dishwasher_programs.h>
#include <driver/gpio.h> // For GPIO_NUM_X definitions
#include <local_time.h>
#include <stdbool.h>
#include <stddef.h> // For size_t
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ring_buffer.h"
#ifndef STEP_ID_FMT
#define STEP_ID_FMT "P=%s C#=%d/%d S#=%d/%d"
#endif

// Debug-time window logger (Recommendation #7)
static inline void log_time_window(const char* phase, time_t start,
                                   time_t min_until, time_t max_until) {
  _LOG_D("%s window: start=%ld min_end=%ld max_end=%ld",
         phase, (long)start, (long)min_until, (long)max_until);
}

RING_BUFFER_DEFINE(prevTemp_rb, int, 16);   // type=int, capacity=16 (power-of-two → fast wrap)

prevTemp_rb temps;  
volatile status_struct ActiveStatus;

static bool verify_program() {
  // Basic verification: check if the program has at least one line
  for (int i = 0; i < NUM_PROGRAMS; i++) {
    _LOG_I("Checking program: %s -> %s", Programs[i].name,
           ActiveStatus.Program);

    if (strcmp(Programs[i].name, ActiveStatus.Program) == 0) {
      ActiveStatus.Active_Program = Programs[i];
      return true;
    }
  }
  return false;
}

void prepare_programs(void) {

  // find chosen program
  Program_Entry current = {0};
  bool found = false;
  long long step_min_time = 0;
  long long step_max_time = 0;
  int cycle = 1;
  char last_cycle[10] = "";

  for (int i = 0; i < NUM_PROGRAMS; i++) {
    long long min_time = 0;
    long long max_time = 0;
    current = Programs[i];
    // compute min/max times and print steps
    for (size_t l = 0; l < current.num_lines; l++) {

      if (strcmp(last_cycle, current.lines[l].name_cycle) != 0) {
        cycle++;
        strcpy(last_cycle, current.lines[l].name_cycle);
      }
      ProgramLineStruct *Line = &current.lines[l];

      step_min_time = (long long)Line->min_time;
      min_time += step_min_time;

      step_max_time =
          (long long)((Line->max_time > 1) ? Line->max_time : Line->min_time);
      max_time += step_max_time;

      _LOG_I("%6s->%6s->%6s\t = Min TTR:%lld\tMax TTR: %lld\tMin Temp:%3d "
             "\tMax Temp:%3d \tGPIO:%" PRIu64,
             SAFE_STR(Programs[i].name), SAFE_STR(Line->name_cycle),
             SAFE_STR(Line->name_step),

             (long long)step_min_time, (long long)step_max_time,

             (int)Line->min_temp, (int)Line->max_temp,
             (uint64_t)Line->gpio_mask);
    }

    Programs[i].min_time = min_time;
    Programs[i].max_time = max_time;
    Programs[i].num_cycles = cycle;
    printf("\nTotal run time for program '%s': Min: %lld Minutes, Max: %lld "
           "Minutes\n",
           current.name, (long long)min_time / MIN, (long long)max_time / MIN);
  }
}
void run_program(void *pvParameters) {
  (void)pvParameters;

  // (#10) Turn DEBUG on for this TAG during the program run; restore to INFO at end
  esp_log_level_set(TAG, ESP_LOG_DEBUG);

  // ---- Verify the program exists ----
  if (!verify_program()) {
    _LOG_E("Invalid program selected: %s", ActiveStatus.Program);
    setCharArray(ActiveStatus.Program, "INVALID");
    esp_log_level_set(TAG, ESP_LOG_INFO);
    vTaskDelete(NULL);
    return;
  }

  Program_Entry *P = &ActiveStatus.Active_Program;

  // ---- Initialize top-level timing/indexes ----
  ActiveStatus.time_full_start = get_unix_epoch();
  ActiveStatus.time_full_total = ActiveStatus.time_full_start + P->max_time;
  ActiveStatus.CyclesTotal     = P->num_cycles;
  ActiveStatus.StepsTotal      = (int32_t)P->num_lines;
  ActiveStatus.CycleIndex      = 0;
  ActiveStatus.StepIndex       = 0;
  gpio_mask_config_outputs(ALL_ACTORS);

  _LOG_D("Program start: %s (cycles=%d steps=%d est_max=%lld)",
         SAFE_STR(ActiveStatus.Program),
         ActiveStatus.CyclesTotal,
         ActiveStatus.StepsTotal,
         (long long)P->max_time);

  // ---- Iterate each program line ----
  const ProgramLineStruct *lines = P->lines;
  char last_cycle[16] = {0};

  for (size_t li = 0; li < P->num_lines; ++li) {
    const ProgramLineStruct *Line = &lines[li];

    // Update indices and labels
    ActiveStatus.StepIndex = (int32_t)(li + 1);
    if (strcmp(SAFE_STR(Line->name_cycle), last_cycle) != 0) {
      ActiveStatus.CycleIndex++;
      ActiveStatus.time_cycle_start = get_unix_epoch();
      ActiveStatus.time_cycle_total = ActiveStatus.time_cycle_start + (Line->min_time ? Line->min_time : 0);
      setCharArray(last_cycle, SAFE_STR(Line->name_cycle));
      _LOG_D("Cycle change -> index=%d name=%s",
             ActiveStatus.CycleIndex, SAFE_STR(Line->name_cycle)); // (#6)
    }
    COPY_STRING(ActiveStatus.Cycle, SAFE_STR(Line->name_cycle));
    COPY_STRING(ActiveStatus.Step,  SAFE_STR(Line->name_step));

    // Reset per-line state
    ActiveStatus.SkipStep        = false;                    // (#5)
    ActiveStatus.HEAT_REACHED    = false;
    ActiveStatus.HEAT_REQUESTED  = ((Line->gpio_mask & HEAT) != 0);
    if (Line->gpio_mask & SOAP) {
      ActiveStatus.SoapHasDispensed = true;
    }

    // Effective actor mask (heat handled separately by thermostat)
    uint64_t actor_mask = (Line->gpio_mask & ALL_ACTORS) & ~HEAT;
    gpio_mask_clear(HEAT | SPRAY | INLET | DRAIN | SOAP);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Base/at-temp constraints
    int base_min = (Line->min_time > 0) ? (int)Line->min_time : 0;
    int base_max = (Line->max_time > 0) ? (int)Line->max_time : base_min; // fallback
    int at_min   = (Line->min_time_at_temp > 0) ? (int)Line->min_time_at_temp : 0;
    int at_max   = (Line->max_time_at_temp > 0) ? (int)Line->max_time_at_temp : 0;

    // (#3) Log constraints (only if relevant)
    if (at_max > 0 && base_min > 0 && at_max < base_min) {
      _LOG_W("Constraint: max_time_at_temp (%d) < min_time (%d).", at_max, base_min);
    }

    const time_t line_start = get_unix_epoch();
    ActiveStatus.LastTransitionMs = line_start;

    // Derived flags for this line (#6)
    bool has_temp_targets = (Line->min_temp > 0) || (Line->max_temp > 0);
    _LOG_D("Step %d/%d: %s %s  HEAT_REQ=%d has_temp=%d "
           "(min=%d max=%d min_at=%d max_at=%d) actor_mask=0x%llx",
           ActiveStatus.StepIndex, ActiveStatus.StepsTotal,
           SAFE_STR(Line->name_cycle), SAFE_STR(Line->name_step),
           (int)ActiveStatus.HEAT_REQUESTED, (int)has_temp_targets,
           Line->min_time, Line->max_time, at_min, at_max,
           (unsigned long long)actor_mask);

    // (#9) Precompute step identity for greppability
    const char* pName = SAFE_STR(ActiveStatus.Program);
    int cIdx = ActiveStatus.CycleIndex, cTot = ActiveStatus.CyclesTotal;
    int sIdx = ActiveStatus.StepIndex,  sTot = ActiveStatus.StepsTotal;

    // Thermostat/hysteresis
    bool  heat_on = false;
    const int maxT     = (Line->max_temp > 0) ? Line->max_temp : 0;
    const int band_low = (maxT > 0) ? (maxT - 3) : 0;
    bool  over_max_warned = false; // (#8) only warn once per line until cooled below band

    // At-temp tracking
    bool   at_temp_started = false;
    time_t at_temp_t0      = 0;
    int    original_remaining_at_hit = 0;
    time_t at_temp_elapsed = 0; // (#4) report at end

    // Base time window
    time_t must_not_end_before = line_start + base_min;
    time_t must_end_by         = line_start + base_max;
    log_time_window("base", line_start, must_not_end_before, must_end_by); // (#3,#7)

    // Assert non-heat actors
    gpio_mask_set(actor_mask);

    // ---- per-line loop ----
    while (true) {
      if (ActiveStatus.SkipStep) {
        ActiveStatus.SkipStep = false;
        _LOG_W("Skipping step on request. " STEP_ID_FMT,
               pName, cIdx, cTot, sIdx, sTot);               // (#5,#9)
        _LOG_D("SkipStep cleared; advancing to next line. " STEP_ID_FMT,
               pName, cIdx, cTot, sIdx, sTot);               // (#5,#9)
        break;
      }

      if (has_temp_targets) {
        // Mark the first time we reach min_temp
        if (!ActiveStatus.HEAT_REACHED && Line->min_temp > 0 &&
            ActiveStatus.CurrentTemp >= Line->min_temp) {
          ActiveStatus.HEAT_REACHED = true;
          _LOG_D("Min temp reached: %dF (threshold=%dF). " STEP_ID_FMT,
                 ActiveStatus.CurrentTemp, Line->min_temp,
                 pName, cIdx, cTot, sIdx, sTot);             // (#6,#9)
        }

        // When we FIRST reach >= min_temp, clamp windows to at-temp spec
        if (!at_temp_started && Line->min_temp > 0 &&
            ActiveStatus.CurrentTemp >= Line->min_temp) {
          at_temp_started = true;
          at_temp_t0 = get_unix_epoch();

          int elapsed = (int)(at_temp_t0 - line_start);
          original_remaining_at_hit = (base_max > 0) ? (base_max - elapsed) : 0;

          time_t new_min_end = at_temp_t0 + (at_min > 0 ? at_min : 0);
          time_t new_max_end = at_temp_t0 + (at_max > 0 ? at_max : 0);

          if (at_min > 0 && base_max > 0 &&
              (at_temp_t0 + at_min) > (line_start + base_max)) {
            _LOG_W("min_time_at_temp (%d) extends past original max_time (%d).",
                   at_min, base_max);                         // (#3)
          }

          if (at_min > 0 && new_min_end > must_not_end_before) {
            must_not_end_before = new_min_end;
          }
          if (at_max > 0) {
            must_end_by = new_max_end;
          }

          int adjusted_remaining = (int)(must_not_end_before - at_temp_t0);
          if (adjusted_remaining < 0) adjusted_remaining = 0;

          _LOG_I("At-temp start @ %ld: original_remaining=%d sec, adjusted_min_remaining=%d sec. "
                 STEP_ID_FMT,
                 (long)at_temp_t0, original_remaining_at_hit, adjusted_remaining,
                 pName, cIdx, cTot, sIdx, sTot);
          log_time_window("at-temp", at_temp_t0, must_not_end_before, must_end_by); // (#3,#7)
        }

        // Track at-temp elapsed for end-of-step report (#4)
        if (at_temp_started) {
          at_temp_elapsed = get_unix_epoch() - at_temp_t0;
        }

        // Over-max warning once per line until cooled below hysteresis band (#8)
        if (maxT > 0) {
          if (!over_max_warned && ActiveStatus.CurrentTemp > maxT) {
            _LOG_W("Max temp EXCEEDED: %s C=%s S=%s max=%dF now=%dF. " STEP_ID_FMT,
                   SAFE_STR(ActiveStatus.Program),
                   SAFE_STR(ActiveStatus.Cycle),
                   SAFE_STR(ActiveStatus.Step),
                   maxT, ActiveStatus.CurrentTemp,
                   pName, cIdx, cTot, sIdx, sTot);
            over_max_warned = true;
          } else if (over_max_warned && ActiveStatus.CurrentTemp <= band_low) {
            // cooled sufficiently; allow a future warning if it spikes again
            over_max_warned = false;
            _LOG_D("Over-max guard reset after cooling (<=%dF). " STEP_ID_FMT,
                   band_low, pName, cIdx, cTot, sIdx, sTot);
          }
        }

        // Hysteresis thermostat around max_temp
        if (maxT > 0) {
          bool desired_heat = false;
          if (ActiveStatus.HEAT_REQUESTED) {
            if (!heat_on) {
              if (ActiveStatus.CurrentTemp <= band_low) desired_heat = true;
            } else {
              desired_heat = (ActiveStatus.CurrentTemp < maxT);
            }
          }

          if (desired_heat && !heat_on) {
            gpio_mask_set(HEAT);
            heat_on = true;
            _LOG_I("HEAT RESUMED (max=%dF band_low=%dF now=%dF). " STEP_ID_FMT,
                   maxT, band_low, ActiveStatus.CurrentTemp,
                   pName, cIdx, cTot, sIdx, sTot);
          } else if (!desired_heat && heat_on) {
            gpio_mask_clear(HEAT);
            heat_on = false;
            _LOG_I("HEAT PAUSED (max=%dF now=%dF). " STEP_ID_FMT,
                   maxT, ActiveStatus.CurrentTemp,
                   pName, cIdx, cTot, sIdx, sTot);
          }
        }
      } else {
        // No temperature targets: force HEAT off
        if (heat_on) { gpio_mask_clear(HEAT); heat_on = false; }
      }

      // Keep non-HEAT actors asserted (refresh)
      gpio_mask_set(actor_mask);

      // Progress log (retain concise I; Ds elsewhere)
      _LOG_I("%8s->%8s:%8s elapsed=%ld sec\tTargettime=%d sec",
             SAFE_STR(ActiveStatus.Program),
             SAFE_STR(Line->name_cycle),
             SAFE_STR(Line->name_step),
             (long)(get_unix_epoch() - line_start),
             (long)(line_max))  ;

      // Exit conditions
      time_t now = get_unix_epoch();
      bool min_satisfied = (now >= must_not_end_before);
      bool max_reached   = (must_end_by > 0 && now >= must_end_by);

      if ((max_reached && min_satisfied) ||
          // if an explicit at_max was set, hitting it ends the step even if min not yet met
          (max_reached && (at_max > 0))) {
        // (#4) reasoned step end
        const char* reason =
          (ActiveStatus.SkipStep) ? "skipped" :
          (max_reached && (at_max > 0)) ? "max_at_reached" :
          (max_reached && min_satisfied) ? "min_and_max_reached" :
          "completed";
        _LOG_D("Step end: reason=%s elapsed=%lds at_temp_elapsed=%lds. " STEP_ID_FMT,
               reason, (long)(now - line_start), (long)at_temp_elapsed,
               pName, cIdx, cTot, sIdx, sTot);
        break;
      }

      vTaskDelay(pdMS_TO_TICKS(5000));
    } // end per-line loop

    // Ensure HEAT off between lines
    if (heat_on) { gpio_mask_clear(HEAT); heat_on = false; }
    // Drop non-heat actors at the end of the line
    gpio_mask_clear(actor_mask);
  }

  _LOG_D("Program complete: %s", SAFE_STR(ActiveStatus.Program));
  esp_log_level_set(TAG, ESP_LOG_INFO); // (#10) restore normal verbosity
  vTaskDelete(NULL);
}

void reset_active_status(void) {
  // Initialize any other fields as necessary
  ActiveStatus.CurrentTemp = 0;
  ActiveStatus.CurrentPower = 0;

  ActiveStatus.time_full_start = 0;
  ActiveStatus.time_full_total = 0;
  ActiveStatus.time_cycle_start = 0;
  ActiveStatus.time_cycle_total = 0;
  ActiveStatus.time_total = 0;
  ActiveStatus.time_elapsed = 0;
  ActiveStatus.time_start = 0;
  ActiveStatus.time_full_start = 0;
  ActiveStatus.time_full_total = 0;
  ActiveStatus.LastTransitionMs = 0;
  ActiveStatus.ProgramStartMs = 0;
  ActiveStatus.ProgramPlannedTotalMs = 0;
  ActiveStatus.ActiveDeviceMask = 0;
  ActiveStatus.StepIndex = 0;
  ActiveStatus.StepsTotal = 0;
  ActiveStatus.CycleIndex = 0;
  ActiveStatus.CyclesTotal = 0;
  ActiveStatus.ActiveDeviceMask = 0;
  // Initialize string fields to empty strings
  ActiveStatus.Cycle[0] = '\0';
  ActiveStatus.Step[0] = '\0';
  ActiveStatus.IPAddress[0] = '\0';
  ActiveStatus.FirmwareStatus[0] = '\0';
  //  ActiveStatus.Program[0] = '\0';
  ActiveStatus.Cycle[0] = '\0';
  ActiveStatus.Step[0] = '\0';
  // ActiveStatus.Program[0] = '\0';
  ActiveStatus.ActiveDevices[0] = '\0';
  ActiveStatus.ActiveLEDs[0] = '\0';
  // Assuming Active_Program is a struct, we need to reset its fields
  // individually

  ActiveStatus.Active_Program.name = NULL;
  ActiveStatus.Active_Program.lines = NULL;
  ActiveStatus.Active_Program.num_lines = 0;
  ActiveStatus.Active_Program.min_time = 0;
  ActiveStatus.Active_Program.max_time = 0;

  ActiveStatus.HEAT_REQUESTED = false;
  ActiveStatus.SoapHasDispensed = false;
  ActiveStatus.SkipStep = false;

  //  bool SoapHasDispensed = false;


}