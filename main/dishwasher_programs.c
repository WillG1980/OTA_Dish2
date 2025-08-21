#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h> 
#include <driver/gpio.h> // For GPIO_NUM_X definitions
#include <stddef.h>      // For size_t
#include <stdint.h>
#include "http_utils.h"
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include "esp_attr.h"
#include "esp_timer.h"   // esp_timer_get_time()
#include "esp_log.h"     // if you prefer ESP_LOGI instead of _LOG_I
#include <local_time.h>
#include <dishwasher_programs.h>

static bool verify_program() {
  // Basic verification: check if the program has at least one line
  for (int i = 0; i < NUM_PROGRAMS; i++) {
    if (strcmp(Programs[i].name, ActiveStatus.Program) == 0) {
      ActiveStatus.Active_Program = Programs[i];
      return true ;     
    }
  }
  return false;
  
}

void prepare_programs(void) {

  // find chosen program
  Program_Entry current = {0};
  bool found = false;
  long long min_time = 0;
  long long max_time = 0;

  for (int i = 0; i < NUM_PROGRAMS; i++) {
    current = Programs[i];
    // compute min/max times and print steps
    for (size_t l = 0; l < current.num_lines; l++) {
      ProgramLineStruct *Line = &current.lines[l];
      min_time += (long long)Line->min_time;
      // if max_time = 0 treat as min_time (as your design did)
      max_time +=
          (long long)((Line->max_time > 0) ? Line->max_time : Line->min_time);

      /* _LOG_I("%6s->%6s->%6s\t = Min TTR:%4" PRIu32 "\tMax TTR:%4" PRIu32
             " \tMin Temp:%3d \tMax Temp:%3d \tGPIO:%" PRIu64,
             SAFE_STR(Programs[i].name), SAFE_STR(Line->name_cycle),
             SAFE_STR(Line->name_step), (uint32_t)Line->min_time/1000,
             (uint32_t)Line->max_time/1000, (int)Line->min_temp, (int)Line->max_temp,
             (uint64_t)Line->gpio_mask);

             */
      /* _LOG_I("%8s->%8s\t->%8s\t = Min TTR: %4.0lld Max TTR: %4.0lld Min Temp %3d
         Max Temp %3d GPIO:%lld", Programs[i].name, Line->name_cycle,
         Line->name_step, Line->min_time, Line->max_time, Line->min_temp,
         Line->max_temp, Line->gpio_mask);
             */
    }

    Programs[i].min_time = min_time;
    Programs[i].max_time = max_time;

    printf("\nTotal run time for program '%s': Min: %lld Minutes, Max: %lld "
           "Minutes\n",
           current.name, (long long)min_time / MIN, (long long)max_time / MIN);
  }
}

void run_program(void *pvParameters) {
  (void)pvParameters;
  _LOG_I("Program selected: %s", ActiveStatus.Program);
  gpio_mask_config_outputs(ALL_ACTORS);
  char *old_cycle = "";
  if(!verify_program()){
    setCharArray(ActiveStatus.Program,"INVALID");
    _LOG_E("Invalid program selected: %s", ActiveStatus.Program);
    vTaskDelete(NULL);

  } // continue on with the program
  
  int64_t cycle_run_time = 0;
  ActiveStatus.time_full_start = get_unix_epoch();
  ActiveStatus.time_full_total = get_unix_epoch() + ActiveStatus.Active_Program.max_time;
  /* Move this under _test_
  _LOG_I("Activating all pins");
  gpio_mask_set(HEAT | SPRAY | INLET | DRAIN ); // set all pins to on for breif test (except SOAP)
  vTaskDelay(pdMS_TO_TICKS(3000));
    _LOG_I("DE-Activating all pins");
  gpio_mask_clear(HEAT | SPRAY | INLET | DRAIN | SOAP); // set all pins to off
  vTaskDelay(pdMS_TO_TICKS(3000));
*/
  for (size_t l = 0; l < ActiveStatus.Active_Program.num_lines; l++) {
    ProgramLineStruct *Line = &ActiveStatus.Active_Program.lines[l];
    gpio_mask_clear(HEAT | SPRAY | INLET | DRAIN | SOAP); // set all pins to off
    old_cycle = Line->name_cycle;
    int TTR =  (Line->max_time > Line->min_time) ? Line->max_time : Line->min_time;
    time_t target_time = get_unix_epoch() + TTR ;
    COPY_STRING(ActiveStatus.Cycle, Line->name_cycle);
    COPY_STRING(ActiveStatus.Step, Line->name_step);
    _LOG_I("Cycle: %s , Step: %s, ActiveStatus.Cycle: %s, ActiveStatus.Step: %s", Line->name_cycle, Line->name_step, ActiveStatus.Cycle, ActiveStatus.Step);
    _LOG_I("%10.8s->%10.8s->%10.8s  TTR:%d: GPIO-mask %lld HARDWARE-MASK: %lld MaskedBits: %s \n", ActiveStatus.Program,Line->name_cycle, Line->name_step, TTR,Line->gpio_mask, HEAT | SPRAY | INLET | DRAIN | SOAP, return_masked_bits(Line->gpio_mask, HEAT | SPRAY | INLET | DRAIN | SOAP));
    gpio_mask_set(Line->gpio_mask); // set all pins to off
    vTaskDelay(pdMS_TO_TICKS(5 * SEC)); // run for 5 seconds minimum

    for (; TTR > 0; TTR -= 5000) 
        {
          gpio_mask_set(Line->gpio_mask); // set all pins to off
          _LOG_D("\t%s:%s\t%d",Line->name_cycle,Line->name_step,TTR);
          vTaskDelay(pdMS_TO_TICKS(5000));
        }
    
    
    /*
    while (target_time < get_unix_epoch()) { // until MAX time reached
      gpio_mask_set(Line->gpio_mask); // set all pins to off
      _LOG_I("Time to run: %d minute -- %s - %s", Line->min_time,
             get_us_time_string(target_time),
             get_us_time_string(get_unix_epoch()));

      vTaskDelay(pdMS_TO_TICKS(5 * SEC)); // Do_runTime
    }
      */
  }

  // TODO: implement actual runtime control of GPIOs, temps, timing, etc.
  // For now block forever (or you could vTaskDelete(NULL) to end the task)
  printf("\nIn final closeout - power cycle to restart/open door\n");

  // never reached
  vTaskDelete(NULL);
}

