// main.c
#ifndef PROJECT_NAME
#define PROJECT_NAME "OTA-Dishwasher"
#endif
#ifndef TAG
#define TAG PROJECT_NAME
#endif
#ifndef APP_VERSION
#define APP_VERSION VERSION
#endif
#include "dishwasher_programs.h" // For BASE_URL and VERSION
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "http_utils.h"
#include "local_wifi.h"
#include "nvs_flash.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #include "analog.h"
#include "buttons.h"
#include "dishwasher_programs.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "local_ota.h"
#include "local_partitions.h"
#include "local_time.h"
#include "local_wifi.h"
#include "logger.h"

static void enter_ship_mode_forever(void) {
  // Stop radios/subsystems (ignore errors if not started)
  esp_wifi_stop();
#if CONFIG_BT_ENABLED
  esp_bt_controller_disable();
#endif

  // Put your pins in safe states here (example only)
  // gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT);
  // gpio_set_level(GPIO_NUM_27, 0);
  // gpio_hold_en(GPIO_NUM_27);           // if you need levels held in deep
  // sleep gpio_deep_sleep_hold_en();           // enable holds across deep
  // sleep

  // Make sure you haven't enabled any wakeups
#if defined(ESP_SLEEP_WAKEUP_ALL)
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#endif
  // (If you enabled specific wakeups earlier, disable them explicitly)

  vTaskDelay(pdMS_TO_TICKS(100)); // let logs flush a moment
  esp_deep_sleep_start();         // won’t return; only EN/power-cycle wakes it
}

#define COPY_STRING(dest, src)                                                 \
  do {                                                                         \
    strncpy((dest), (src), sizeof(dest) - 1);                                  \
    (dest)[sizeof(dest) - 1] = '\0';                                           \
  } while (0)
// global status
status_struct ActiveStatus;
#include "esp_system.h"

// prototypes (task functions must be of type void f(void *))
static void monitor_task_buttons(void *pvParameters);
static void monitor_task_temperature(void *pvParameters);
static void update_published_status(void *pvParameters);
static void run_program(void *pvParameters);
static void _init_setup(void);
static void init_status();
void print_status();

// ----- implementations -----
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

      _LOG_I("%s->%s\t->%s\t = Min TTR:%4" PRIu32 "  Max TTR:%4" PRIu32
             "  Min Temp:%3d  Max Temp:%3d  GPIO:%" PRIu64,
             SAFE_STR(Programs[i].name), SAFE_STR(Line->name_cycle),
             SAFE_STR(Line->name_step), (uint32_t)Line->min_time,
             (uint32_t)Line->max_time, (int)Line->min_temp, (int)Line->max_temp,
             (uint64_t)Line->gpio_mask);

      /* _LOG_I("%s->%s\t->%s\t = Min TTR: %4.0lld Max TTR: %4.0lld Min Temp %3d
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

static void _init_setup(void) {
  // initialize subsystems (these functions should be provided by their
  // modules)
  local_wifi_init_and_connect();
  logger_init("10.0.0.123", 5000, 4096);
  logger_flush();

  int counter = 60;
  while (counter > 0) {
    _LOG_I("waiting on wifi... %d remaining, status %d", counter,
           is_connected());
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (is_connected()) {
      counter = -1;
    }
    counter--;
  }

  check_and_perform_ota();
  while (strcasecmp(ActiveStatus.Program, "Updating") == 0) {
    vTaskDelay(pdMS_TO_TICKS(30 * SEC));
  }
  initialize_sntp_blocking();
  init_switchesandleds();
  logger_init("10.0.0.123", 5000, 4096);
  logger_flush();
  init_status();
  print_status();
  prepare_programs();
  //  vTaskDelay(pdMS_TO_TICKS(1000000));

  // create background monitoring tasks (use reasonable stack sizes)
  xTaskCreate(monitor_task_buttons, "monitor_task_buttons", 4096, NULL, 5,
              NULL);
  xTaskCreate(monitor_task_temperature, "monitor_task_temperature", 4096, NULL,
              5, NULL);
  xTaskCreate(update_published_status, "update_published_status", 4096, NULL, 5,
              NULL);
  // wait (up to 60s) for wifi
  for (int t = 60; t > 0; t--) {
    _LOG_I("Waiting for wifi, %d seconds remaining", t);
    if (is_connected()) {
      _LOG_I("Connected to Wifi");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
static void monitor_task_buttons(void *pvParameters) {
  (void)pvParameters;
  // TODO: implement real button monitoring
  while (1) {
    // poll or wait on interrupts/queue
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
static void monitor_task_temperature(void *pvParameters) {
  (void)pvParameters;
  // TODO: implement real temperature monitoring
  while (1) {
    // sample ADC/thermistor
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
void print_status() {
  printf("\nStatus update: State: %s/%s\
          \n\tTemperature: %d\
          \n\tElapsed Time(full):\t%lld \tFull ETA: %lld\
          \n\tElapsed Time(Cycle):\t%lld \tCycle ETA: %lld\
          \n\tIP: %s\n",
         ActiveStatus.Cycle, ActiveStatus.Step, ActiveStatus.CurrentTemp,
         (long long)(get_unix_epoch() - ActiveStatus.time_full_start),
         (long long)ActiveStatus.time_full_total,
         (long long)(get_unix_epoch() - ActiveStatus.time_cycle_start),
         (long long)ActiveStatus.time_cycle_total, ActiveStatus.IPAddress);
};
static void update_published_status(void *pvParameters) {
  (void)pvParameters;
  _LOG_I("Starting");
  while (1) {
    int count = 0;
    if (strcmp(ActiveStatus.Cycle, "Off") == 0) {
      _LOG_I("Cycle-Off");
      vTaskDelay(pdMS_TO_TICKS(30000));
      count++;
      if (count > 10) {
        count = 0;
        printf("%s, Dishwasher is OFF; Dishes are in DIRTY state",
               ActiveStatus.Cycle);
      };
    } else if (strcmp(ActiveStatus.Cycle, "fini") == 0) {
      _LOG_I("Cycle-Finished");
      vTaskDelay(pdMS_TO_TICKS(30000));
      count++;
      if (count > 10) {
        count = 0;
        printf("%s, Dishwasher is OFF; Dishes are in CLEAN state",
               ActiveStatus.Cycle);
      };
    } else {
      _LOG_I("Cycle-Processing");
      print_status();
      vTaskDelay(pdMS_TO_TICKS(30000));
    }
  }
  _LOG_I("Finishing");
} // run_program task: summarises and then blocks

static void run_program(void *pvParameters) {
  (void)pvParameters;
  gpio_mask_config_outputs(ALL_ACTORS);

  /* // Group mask    uint64_t mask = HEAT | SPRAY | INLET | DRAIN | SOAP;
      // Turn them all ON (HIGH)    gpio_port_set_mask(GPIO_PORT_0, mask);
      // Turn them all OFF (LOW)    gpio_port_clear_mask(GPIO_PORT_0, mask);
      // Toggle them    gpio_port_toggle_mask(GPIO_PORT_0, mask);
  }*/

  char *old_cycle = "";
  Program_Entry chosen = {0};
  bool found = false;

  for (int i = 0; i < NUM_PROGRAMS; i++) {
    if (strcmp(Programs[i].name, ActiveStatus.Program) == 0) {
      chosen = Programs[i];
      found = true;
      break;
    }
  }
  if (!found) {
    _LOG_W("Program '%s' not found. Exiting run_program task.",
           ActiveStatus.Program);
    vTaskDelete(NULL);
    return;
  }

  // Actually run cycle(s)
  int64_t cycle_run_time = 0;
  ActiveStatus.time_full_start = get_unix_epoch();
  //    ActiveStatus.time_full_total = get_unix_epoch() + max_time;

  _LOG_I("Activating all pins");
  gpio_mask_set(HEAT | SPRAY | INLET | DRAIN | SOAP); // set all pins to off
  vTaskDelay(pdMS_TO_TICKS(3000));
  _LOG_I("DE-Activating all pins");
  gpio_mask_clear(HEAT | SPRAY | INLET | DRAIN | SOAP); // set all pins to off
  vTaskDelay(pdMS_TO_TICKS(3000));

  for (size_t l = 0; l < chosen.num_lines; l++) {
    ProgramLineStruct *Line = &chosen.lines[l];

    if (strcmp(old_cycle, Line->name_cycle) != 0) {
      printf("\n-- new cycle: %s --\n", Line->name_cycle);
    }
    old_cycle = Line->name_cycle;
    int TTR =
        (Line->max_time > Line->min_time) ? Line->max_time : Line->min_time;

    time_t target_time = get_unix_epoch() + TTR * MIN;
    COPY_STRING(ActiveStatus.Cycle, Line->name_cycle);
    COPY_STRING(ActiveStatus.Step, Line->name_step);

    _LOG_I("\n%s:%s->%s: Eta %s GPIO-mask %lld\n", ActiveStatus.Program,
           Line->name_cycle, Line->name_step, get_us_time_string(target_time),
           return_masked_bits(Line->gpio_mask, ALL_ACTORS));

    vTaskDelay(pdMS_TO_TICKS(5 * SEC)); // run for 5 seconds minimum

    while (target_time < get_unix_epoch()) { // until MAX time reached

      gpio_mask_set(Line->gpio_mask); // set all pins to off
      _LOG_I("Time to run: %d minute -- %s - %s", Line->min_time,
             get_us_time_string(target_time),
             get_us_time_string(get_unix_epoch()));

      vTaskDelay(pdMS_TO_TICKS(5 * SEC)); // Do_runTime
    }
  }

  // TODO: implement actual runtime control of GPIOs, temps, timing, etc.
  // For now block forever (or you could vTaskDelete(NULL) to end the task)
  printf("\nIn final closeout - power cycle to restart/open door\n");

  // never reached
  vTaskDelete(NULL);
}

void init_status(void) {

  _LOG_I("Starting Function");
  ActiveStatus.CurrentPower = 0;
  ActiveStatus.CurrentTemp = 0;
  setCharArray(ActiveStatus.Cycle, "Off");
  setCharArray(ActiveStatus.Step, "Off");
  setCharArray(ActiveStatus.IPAddress, "255.255.255.255");

  ActiveStatus.time_full_start = 0;
  ActiveStatus.time_full_total = 0;
  ActiveStatus.time_cycle_start = 0;
  ActiveStatus.time_cycle_total = 0;
  ActiveStatus.time_elapsed = 0;
  _LOG_I("Ending Function");
}
// app_main

void app_main(void) {
  _LOG_I("Booting: %s", boot_partition_cstr());
  _LOG_I("Running: %s", running_partition_cstr());
  _LOG_I("Version: %s", APP_VERSION);

  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi*", ESP_LOG_WARN);
  esp_log_level_set("phy", ESP_LOG_WARN);
  esp_log_level_set("ota_dishwasher", ESP_LOG_VERBOSE);

  printf("Version: %s\n", APP_VERSION);
  ESP_ERROR_CHECK(nvs_flash_init());
  _init_setup();

  check_and_perform_ota();

  printf("\n\tTotal program count: %d\n", NUM_PROGRAMS);
  for (int i = 0; i < NUM_PROGRAMS; i++) {
    printf("\n\t\tProgram Name: %s\n", Programs[i].name);
  }
  // start wifi, monitors, etc
  // choose program and start program task
  setCharArray(ActiveStatus.Program, "Tester");
  _LOG_I("Queueing a new wash task task");

  xTaskCreate(run_program, "Run_Program", 8192, NULL, 5, NULL);

  vTaskDelay(pdMS_TO_TICKS(10000));

  // Keep main alive but yield CPU — do not busy-loop
  while (1) {

    if (strcmp(ActiveStatus.Cycle, "fini") == 0) {
      log_uptime_hms();
      enter_ship_mode_forever();
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
