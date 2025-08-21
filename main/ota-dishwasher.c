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
#include <http_server.h>


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

#include "esp_system.h"


static QueueHandle_t action_queue;


// prototypes (task functions must be of type void f(void *))
static void monitor_task_buttons(void *pvParameters);
static void monitor_task_temperature(void *pvParameters);
static void update_published_status(void *pvParameters);
static void run_program(void *pvParameters);
static void _init_setup(void);
static void init_status();
void print_status();
static void net_probe(const char* ip, uint16_t port) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in to = {.sin_family=AF_INET, .sin_port=htons(port)};
    inet_aton(ip, &to.sin_addr);
    const char *msg = "ESP UDP probe\n";
    sendto(s, msg, strlen(msg), 0, (struct sockaddr*)&to, sizeof(to));
    close(s);
}
// ----- implementations -----
static void _init_setup(void) {
  // initialize subsystems (these functions should be provided by their
  // modules)
  local_wifi_init_and_connect();
  logger_init("10.0.0.123", 5514, 4096);
  http_server_actions_init();
  //logger_flush();

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

   
  while (strcasecmp(ActiveStatus.Program, "Updating") == 0) {
    vTaskDelay(pdMS_TO_TICKS(30 * SEC));
  }
  initialize_sntp_blocking();
  init_switchesandleds();
  net_probe("10.0.0.123",5514);
      //logger_flush();
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
  /* printf("\nStatus update: State: %s/%s\
          \n\tTemperature: %d\
          \n\tElapsed Time(full):\t%lld \tFull ETA: %lld\
          \n\tElapsed Time(Cycle):\t%lld \tCycle ETA: %lld\
          \n\tIP: %s\n",
         ActiveStatus.Cycle, ActiveStatus.Step, ActiveStatus.CurrentTemp,
         (long long)(get_unix_epoch() - ActiveStatus.time_full_start),
         (long long)ActiveStatus.time_full_total,
         (long long)(get_unix_epoch() - ActiveStatus.time_cycle_start),
         (long long)ActiveStatus.time_cycle_total, ActiveStatus.IPAddress);
*/
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
   //   _LOG_I("Cycle-Processing");
      print_status();
      vTaskDelay(pdMS_TO_TICKS(30000));
    }
  }
  _LOG_I("Finishing");
} // run_program task: summarises and then blocks




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
httpd_handle_t server = NULL;
void app_main(void) {
  _LOG_I("Booting: %s", boot_partition_cstr());
  _LOG_I("Running: %s", running_partition_cstr());
  _LOG_I("Version: %s", APP_VERSION);

  esp_log_level_set("*", ESP_LOG_DEBUG);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi*", ESP_LOG_WARN);
  esp_log_level_set("phy", ESP_LOG_WARN);
  esp_log_level_set("ota_dishwasher", ESP_LOG_VERBOSE);

  printf("Version: %s\n", APP_VERSION);
  ESP_ERROR_CHECK(nvs_flash_init());
  _init_setup();

  server = start_webserver();
  check_and_perform_ota();

  printf("\n\tTotal program count: %d\n", NUM_PROGRAMS);
  for (int i = 0; i < NUM_PROGRAMS; i++) {
    printf("\n\t\tProgram Name: %s\n", Programs[i].name);
  }
  // start wifi, monitors, etc
  // choose program and start program task
  setCharArray(ActiveStatus.Program, "Tester");
  _LOG_I("Queueing a new wash task task");


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
