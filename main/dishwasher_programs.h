#ifndef DISHWASHER_PROGRAM_H
#define DISHWASHER_PROGRAM_H

#ifndef PROJECT_NAME
#define PROJECT_NAME "OTA-Dishwasher"
#endif

#ifndef TAG
#define TAG PROJECT_NAME
#endif

#ifndef APP_VERSION
#define APP_VERSION VERSION
#endif

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
#include "io.h"
#include "nvs_flash.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include <driver/gpio.h> // For GPIO_NUM_X definitions
#include <local_time.h>
#include <stdbool.h>
#include <stddef.h> // For size_t
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _LOG_I(fmt, ...)                                                       \
  ESP_LOGI(TAG, "[Ver:%s %s:%s:%d]=\t" fmt, APP_VERSION, __FILE__, __func__,   \
           __LINE__, ##__VA_ARGS__)
#define _LOG_W(fmt, ...)                                                       \
  ESP_LOGW(TAG, "[Ver:%s %s:%s:%d]=\t" fmt, APP_VERSION, __FILE__, __func__,   \
           __LINE__, ##__VA_ARGS__)
#define _LOG_E(fmt, ...)                                                       \
  ESP_LOGE(TAG, "[Ver:%s %s:%s:%d]=\t" fmt, APP_VERSION, __FILE__, __func__,   \
           __LINE__, ##__VA_ARGS__)
#define _LOG_D(fmt, ...)                                                       \
  ESP_LOGD(TAG, "[Ver:%s %s:%s:%d]=\t" fmt, APP_VERSION, __FILE__, __func__,   \
           __LINE__, ##__VA_ARGS__)

#define COPY_STRING(dest, src)                                                 \
  do {                                                                         \
    strncpy((dest), (src), sizeof(dest) - 1);                                  \
    (dest)[sizeof(dest) - 1] = '\0';                                           \
  } while (0)

void run_program(void *pvParameters);

void prepare_programs();

static inline void log_uptime_hms(void) {
  int64_t us = esp_timer_get_time(); // microseconds since boot
  int64_t s = us / 1000000LL;

  int64_t h = s / 3600;
  int m = (int)((s % 3600) / 60);
  int t = (int)(s % 60);

  _LOG_I("Uptime: %lld:%02d:%02d", (long long)h, m, t);
  // Or: ESP_LOGI("UPTIME", "Uptime: %lld:%02d:%02d", (long long)h, m, t);
}
// Configure all bits in mask as outputs (call once at init)
static inline void gpio_mask_config_outputs(uint64_t mask) {
  gpio_config_t io = {.pin_bit_mask = mask,
                      .mode = GPIO_MODE_OUTPUT,
                      .pull_down_en = GPIO_PULLDOWN_DISABLE,
                      .pull_up_en = GPIO_PULLUP_DISABLE,
                      .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&io);
}
// Set HIGH for all pins in mask
static inline void IRAM_ATTR gpio_mask_set(uint64_t mask) {
  uint32_t lo = (uint32_t)mask;
  uint32_t hi = (uint32_t)(mask >> 32);
  if (lo)
    REG_WRITE(GPIO_OUT_W1TS_REG, lo);
  if (hi)
    REG_WRITE(GPIO_OUT1_W1TS_REG, hi);
}
// Set LOW for all pins in mask
static inline void IRAM_ATTR gpio_mask_clear(uint64_t mask) {
  uint32_t lo = (uint32_t)mask;
  uint32_t hi = (uint32_t)(mask >> 32);
  if (lo)
    REG_WRITE(GPIO_OUT_W1TC_REG, lo);
  if (hi)
    REG_WRITE(GPIO_OUT1_W1TC_REG, hi);
}
// Write level to all pins in mask (level: 0/1)
static inline void IRAM_ATTR gpio_mask_write(uint64_t mask, bool level) {
  if (level)
    gpio_mask_set(mask);
  else
    gpio_mask_clear(mask);
}
// Toggle all pins in mask
static inline void IRAM_ATTR gpio_mask_toggle(uint64_t mask) {
  uint32_t lo = (uint32_t)mask;
  uint32_t hi = (uint32_t)(mask >> 32);

  if (lo) {
    uint32_t out_lo = GPIO.out; // current low 32 bits
    REG_WRITE(GPIO_OUT_W1TS_REG, (~out_lo) & lo);
    REG_WRITE(GPIO_OUT_W1TC_REG, (out_lo)&lo);
  }
  if (hi) {
    uint32_t out_hi = GPIO.out1.val; // current high 32 bits
    REG_WRITE(GPIO_OUT1_W1TS_REG, (~out_hi) & hi);
    REG_WRITE(GPIO_OUT1_W1TC_REG, (out_hi)&hi);
  }
}

#ifndef BIT64
#define BIT64(n) (1ULL << (n))
#endif

#define HEAT (BIT64(GPIO_NUM_32))
#define SPRAY (BIT64(GPIO_NUM_33))
#define INLET (BIT64(GPIO_NUM_25))
#define DRAIN (BIT64(GPIO_NUM_26))
#define SOAP (BIT64(GPIO_NUM_27))

#define NUM_LEDS 8
static const uint64_t ALL_ACTORS = HEAT | SPRAY | INLET | DRAIN | SOAP;
#define NUM_PROGRAMS 3

#define SEC (1) // 1 second is one second
#define MIN (60)   // 60 seconds in one minute
#define SAFE_STR(p) ((p) ? (p) : "")
#define NUM_DEVICES 8

typedef struct {
  char *name_cycle;
  char *name_step;
  uint32_t min_time;
  uint32_t max_time;
  int min_temp;
  int max_temp;
  uint64_t gpio_mask; // BIT64 mask for all pins to set HIGH
} ProgramLineStruct;

typedef struct {
  const char *name;
  const ProgramLineStruct *lines;
  size_t num_lines;
  int64_t min_time;
  int64_t max_time;
  int num_cycles;  
} Program_Entry;

// Normal program
static const ProgramLineStruct NormalProgramLines[] = {
    {"init", "setup", 1, 0, 0, 0, 0},

    {"Prep", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"Prep", "Spray", 5 * MIN, 0, 0, 0, SPRAY},
    {"Prep", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"wash", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"wash", "Warm", 5 * MIN, 40 * MIN, 130, 140,
     HEAT | SPRAY}, // heat water to at _least_ 130
    {"wash", "soap", 1 * MIN, 0, 140, 150, HEAT | SPRAY | SOAP},
    {"wash", "wash", 45 * MIN, 75 * MIN, 150, 150, HEAT | SPRAY},
    {"wash", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse1", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"rinse1", "rinse", 5 * MIN, 0, 0, 0, HEAT | SPRAY},
    {"rinse1", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse2", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"rinse2", "rinse", 5 * MIN, 0, 0, 0, HEAT | SPRAY},
    {"rinse2", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse3", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"rinse3", "soap", 1 * MIN, 0, 140, 140, HEAT | DRAIN | SOAP}, // rinse aid
    {"rinse3", "rinse", 10 * MIN, 20 * MIN, 140, 140, HEAT | SPRAY},
    {"rinse3", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"cool", "vent", 29 * MIN, 0, 0, 0, HEAT},
    {"fini", "clean", 0, 0, 0, 0, 0}};
// Test program: all times 30 seconds, temps copied from Normal
static const ProgramLineStruct TesterProgramLines[] = {
    {"init", "setup", 1, 0, 0, 0, 0},

    {"Prep", "fill", 30 * SEC, 0, 0, 0, INLET},
    {"Prep", "Spray", 30 * SEC, 30 * SEC, 130, 130, SPRAY},
    {"Prep", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"wash", "fill", 30 * SEC, 0, 0, 0, INLET},
    {"wash", "Warm", 0, 30 * SEC, 130, 130, HEAT | SPRAY},
    {"wash", "soap", 30 * SEC, 0, 140, 140, HEAT | SPRAY | SOAP},
    {"wash", "wash", 30 * SEC, 30 * SEC, 152, 152, HEAT | SPRAY},
    {"wash", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse1", "fill", 30 * SEC, 0, 0, 0, INLET},
    {"rinse1", "rinse", 30 * SEC, 0, 0, 0, HEAT | SPRAY},
    {"rinse1", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse2", "fill", 30 * SEC, 0, 0, 0, INLET},
    {"rinse2", "rinse", 30 * SEC, 0, 0, 0, HEAT | SPRAY},
    {"rinse2", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse3", "fill", 30 * SEC, 0, 0, 0, INLET},
    {"rinse3", "soap", 30 * SEC, 0, 140, 140, HEAT | DRAIN | SOAP},
    {"rinse3", "rinse", 30 * SEC, 30 * SEC, 140, 140, HEAT | SPRAY},
    {"rinse3", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"cool", "vent", 29 * MIN, 0, 0, 0, HEAT},
    {"fini", "clean", 0, 0, 0, 0, 0}

};
// Hi-Temp program: same times as Normal, but all wash temps set to 160
static const ProgramLineStruct HiTempProgramLines[] = {
    {"init", "setup", 1, 0, 0, 0, 0},

    {"Prep", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"Prep", "Spray", 5 * MIN, 0, 0, 0, SPRAY},
    {"Prep", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"wash", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"wash", "Warm", 0, 40 * MIN, 160, 160, HEAT | SPRAY},
    {"wash", "soap", 1 * MIN, 0, 160, 160, HEAT | SPRAY | SOAP},
    {"wash", "wash", 45 * MIN, 75 * MIN, 160, 160, HEAT | SPRAY},
    {"wash", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse1", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"rinse1", "rinse", 5 * MIN, 0, 0, 0, HEAT | SPRAY},
    {"rinse1", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse2", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"rinse2", "rinse", 5 * MIN, 0, 0, 0, HEAT | SPRAY},
    {"rinse2", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"rinse3", "fill", 3 * MIN, 0, 0, 0, INLET},
    {"rinse3", "soap", 1 * MIN, 0, 160, 160, HEAT | DRAIN | SOAP},
    {"rinse3", "rinse", 10 * MIN, 20 * MIN, 160, 160, HEAT | SPRAY},
    {"rinse3", "drain", 2 * MIN, 0, 0, 0, DRAIN},

    {"cool", "vent", 29 * MIN, 0, 140, 140, HEAT},
    {"fini", "clean", 0, 0, 0, 0, 0}};
// Cancel program. drain tank, shut down.
static const ProgramLineStruct CancelProgramLines[] = {
    {"Cancel", "drain", 2 * MIN, 0, 0, 0, DRAIN},
    {"fini", "clean", 0, 0, 0, 0, 0}};
static Program_Entry Programs[NUM_PROGRAMS] = {
    {"Tester", TesterProgramLines,
     sizeof(TesterProgramLines) / sizeof(TesterProgramLines[0]),0},
    {"Normal", NormalProgramLines,
     sizeof(NormalProgramLines) / sizeof(NormalProgramLines[0]),0},
    {"HiTemp", HiTempProgramLines,
     sizeof(HiTempProgramLines) / sizeof(HiTempProgramLines[0]),0},
      {"Cancel", CancelProgramLines,
     sizeof(CancelProgramLines) / sizeof(CancelProgramLines[0]),0};
#define setCharArray(target, value)                                            \
  do {                                                                         \
    strncpy((target), (value), sizeof(target) - 1);                            \
    (target)[sizeof(target) - 1] = '\0';                                       \
  } while (0)

// Print bits from "value" according to which positions are flagged in "mask"
static inline const char *return_masked_bits(uint64_t value, uint64_t mask) {
  static char s[65];
  size_t i = 0;

  for (int bit = 63; bit >= 0; --bit) {
    if (mask & (1ULL << bit)) {
      if (i < 64) {
        s[i++] = (char)('0' + (int)((value >> bit) & 1ULL));
      }
    }
  }
  s[i] = '\0';
  return s;
}
static inline void print_masked_bits(uint64_t value, uint64_t mask) {
  for (int bit = 63; bit >= 0; bit--) {
    if (mask & (1ULL << bit)) {
      printf("%d", (int)((value >> bit) & 1ULL));
    }
  }
  printf("\n");
}
static inline void delay_monitor(int64_t millis, int64_t time_between_beats) {
  int counter = (millis + time_between_beats - 1) / time_between_beats;
  int loops = 0;
  int wait = 0;
  _LOG_I("Counter Loops: %d", counter);

  for (; millis > time_between_beats; millis = -time_between_beats) {
    counter++;
    if (counter % 10) {
      printf("\n");
    }
    wait = (millis > time_between_beats) ? time_between_beats : millis;
    printf(". %d \t-- %lld -- %lld ", wait, millis, time_between_beats);

    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

#ifndef ActiveStatus_defined

typedef struct {
  int CurrentTemp;
  int CurrentPower;
  int64_t time_full_start;
  int64_t time_full_total;
  int64_t time_cycle_start;
  int64_t time_cycle_total;
  int64_t time_total;
  int64_t time_elapsed;
  int64_t time_start;
  char Cycle[10];
  char Step[10];
  char IPAddress[16];     // OPTIMIZATION: Fixed size for IP
  char FirmwareStatus[20];
  char Program[10];
  bool HEAT_REQUESTED;
  char ActiveDevices[NUM_DEVICES]; // result of return_masked_bits
  char ActiveLEDs[NUM_LEDS];       // bitmask of active LED names
  bool SoapHasDispensed;

  uint64_t ActiveDeviceMask; // BIT64 mask of all active devices
  int32_t StepIndex;
  int32_t StepsTotal;
  int32_t CycleIndex;
  int32_t CyclesTotal;
  int64_t LastTransitionMs;
  int64_t ProgramStartMs; 
  int64_t ProgramPlannedTotalMs;

  Program_Entry Active_Program;

} status_struct;
#define ActiveStatus_defined
extern volatile status_struct ActiveStatus;

#endif

#endif