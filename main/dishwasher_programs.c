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

  _LOG_I("Program selected: %s", ActiveStatus.Program);
  gpio_mask_config_outputs(ALL_ACTORS);
  char old_cycle[6] = "\n";

  if (!verify_program()) {
    setCharArray(ActiveStatus.Program, "INVALID");
    _LOG_E("Invalid program selected: %s", ActiveStatus.Program);
    vTaskDelete(NULL);

  } // continue on with the program

  int64_t cycle_run_time = 0;
  ActiveStatus.time_full_start = get_unix_epoch();
  ActiveStatus.time_full_total =
      get_unix_epoch() + ActiveStatus.Active_Program.max_time;
  ActiveStatus.CyclesTotal = ActiveStatus.Active_Program.num_cycles;
  ActiveStatus.StepsTotal = ActiveStatus.Active_Program.num_lines;

  for (size_t l = 0; l < ActiveStatus.Active_Program.num_lines; l++) {
    ActiveStatus.StepIndex = l + 1;

    ProgramLineStruct *Line = &ActiveStatus.Active_Program.lines[l];

    if (strcmp(Line->name_cycle, old_cycle) != 0) {
      // new cycle
      ActiveStatus.CycleIndex++;
      ActiveStatus.time_cycle_start = get_unix_epoch();
      ActiveStatus.time_cycle_total =
          get_unix_epoch() + Line->min_time; // rough estimate
      setCharArray(old_cycle, Line->name_cycle);
    }
    gpio_mask_clear(HEAT | SPRAY | INLET | DRAIN | SOAP); // set all pins to off

    int TTR =
        (Line->max_time > Line->min_time) ? Line->max_time : Line->min_time;
    time_t target_time = get_unix_epoch() + TTR;

    COPY_STRING(ActiveStatus.Cycle, Line->name_cycle);
    COPY_STRING(ActiveStatus.Step, Line->name_step);

    _LOG_I("%8.8s->%8.8s->%8.8s  TTR:%d: MaskedBits: %s \n",
           ActiveStatus.Program, Line->name_cycle, Line->name_step, TTR,
           return_masked_bits(Line->gpio_mask,
                              HEAT | SPRAY | INLET | DRAIN | SOAP));
    uint64_t gpio_mask = Line->gpio_mask;
    gpio_mask = gpio_mask & ALL_ACTORS; // only allow valid actors
    ActiveStatus.HEAT_REQUESTED = (gpio_mask & HEAT) ? true : false;
    gpio_mask &= ~HEAT;                 // remove HEAT, handle differently
    gpio_mask_set(gpio_mask);           // set all pins to off
    vTaskDelay(pdMS_TO_TICKS(5 * SEC)); // run for 5 seconds minimum
/*

prevTemp_rb_clear(&temps);
    for (int i = 100; i < 120; ++i) prevTemp_rb_push(&temps, i);

    int newest = prevTemp_rb_recent(&temps, 0);   // newest
    int oldest = prevTemp_rb_recent(&temps, prevTemp_rb_size(&temps)-1);
    double avg = prevTemp_rb_average(&temps);
    
    */
    for (; TTR > 0; TTR -= 5) {
      if (ActiveStatus.SkipStep) {
        ActiveStatus.SkipStep = false;
        _LOG_W("Skipping step as requested");
        break;
      }
      if (ActiveStatus.HEAT_REQUESTED) {
        prevTemp_rb_push(&temps, ActiveStatus.CurrentTemp);

        if ( (prevTemp_rb_recent(&temps, 1)>(ActiveStatus.CurrentTemp+2)) || (prevTemp_rb_recent(&temps, 1)<(ActiveStatus.CurrentTemp-2))) {
          _LOG_I("Temperature changed more then 2 degrees in 5 seconds Current %d Past %d",prevTemp_rb_recent(&temps,1), ActiveStatus.CurrentTemp);

        }

        if (ActiveStatus.CurrentTemp < Line->max_temp) {
          _LOG_I("Turning HEAT ON: Current/Target Temp: %d / %d ",
                 ActiveStatus.CurrentTemp, Line->max_temp);
          gpio_mask_set(HEAT);
        } else {
          _LOG_I("Leaving HEAT OFF %d / %d ",
                 ActiveStatus.CurrentTemp, Line->max_temp);
          gpio_mask_clear(HEAT);
        }

      } else {
        gpio_mask_clear(HEAT);
      }

      gpio_mask_set(
          Line->gpio_mask); // set all pins to on every 5 seconds to be safe
      _LOG_I("\t%8s->%8s:%8s\t%d", ActiveStatus.Program, Line->name_cycle,
             Line->name_step, TTR);
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
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