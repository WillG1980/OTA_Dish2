#ifndef DISHWASHER_PROGRAM_H
#define DISHWASHER_PROGRAM_H


#ifndef PROJECT_NAME
#define PROJECT_NAME "OTA-Dishwasher"
#endif

#ifndef TAG
#define TAG "PROJECT_NAME "
#endif

#ifndef APP_VERSION
#define APP_VERSION VERSION
#endif

 
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

// Configure all bits in mask as outputs (call once at init)
static inline void gpio_mask_config_outputs(uint64_t mask) {
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
}

// Set HIGH for all pins in mask
static inline void IRAM_ATTR gpio_mask_set(uint64_t mask) {
    uint32_t lo = (uint32_t)mask;
    uint32_t hi = (uint32_t)(mask >> 32);
    if (lo) REG_WRITE(GPIO_OUT_W1TS_REG, lo);
    if (hi) REG_WRITE(GPIO_OUT1_W1TS_REG, hi);
}

// Set LOW for all pins in mask
static inline void IRAM_ATTR gpio_mask_clear(uint64_t mask) {
    uint32_t lo = (uint32_t)mask;
    uint32_t hi = (uint32_t)(mask >> 32);
    if (lo) REG_WRITE(GPIO_OUT_W1TC_REG, lo);
    if (hi) REG_WRITE(GPIO_OUT1_W1TC_REG, hi);
}

// Write level to all pins in mask (level: 0/1)
static inline void IRAM_ATTR gpio_mask_write(uint64_t mask, bool level) {
    if (level) gpio_mask_set(mask); else gpio_mask_clear(mask);
}

// Toggle all pins in mask
static inline void IRAM_ATTR gpio_mask_toggle(uint64_t mask) {
    uint32_t lo = (uint32_t)mask;
    uint32_t hi = (uint32_t)(mask >> 32);

    if (lo) {
        uint32_t out_lo = GPIO.out;                 // current low 32 bits
        REG_WRITE(GPIO_OUT_W1TS_REG, (~out_lo) & lo);
        REG_WRITE(GPIO_OUT_W1TC_REG, ( out_lo) & lo);
    }
    if (hi) {
        uint32_t out_hi = GPIO.out1.val;            // current high 32 bits
        REG_WRITE(GPIO_OUT1_W1TS_REG, (~out_hi) & hi);
        REG_WRITE(GPIO_OUT1_W1TC_REG, ( out_hi) & hi);
    }
}




#define _LOG_I(fmt, ...)   ESP_LOGI(TAG, "[%s-%s:%d]= " fmt, __func__, APP_VERSION, __LINE__, ##__VA_ARGS__)
#define _LOG_W(fmt, ...)   ESP_LOGW(TAG, "[%s-%s:%d]= " fmt, __func__, APP_VERSION, __LINE__, ##__VA_ARGS__)
#define _LOG_E(fmt, ...)   ESP_LOGE(TAG, "[%s-%s:%d]= " fmt, __func__, APP_VERSION, __LINE__, ##__VA_ARGS__)
#define _LOG_D(fmt, ...)   ESP_LOGD(TAG, "[%s-%s:%d]= " fmt, __func__, APP_VERSION, __LINE__, ##__VA_ARGS__)

#ifndef BIT64
#define BIT64(n) (1ULL << (n))
#endif



#define HEAT (BIT64(GPIO_NUM_32))
#define SPRAY (BIT64(GPIO_NUM_33))
#define INLET (BIT64(GPIO_NUM_25))
#define DRAIN (BIT64(GPIO_NUM_26))
#define SOAP (BIT64(GPIO_NUM_27))
#define SENSOR_ENABLE (BIT64(GPIO_NUM_18))
#define CLEANLIGHT (BIT64(GPIO_NUM_19))
#define LIGHT3 (BIT64(GPIO_NUM_21))

static const uint64_t ALL_ACTORS = HEAT | SPRAY | INLET | DRAIN | SOAP;

#define NUM_PROGRAMS 3

#define SEC (1ULL)     // 1 second in milliseconds
#define MIN (60 * SEC) // 60 seconds in milliseconds
#define SAFE_STR(p) ((p) ? (p) : "")
#define NUM_DEVICES 8

//static const char *FIRMWARE_URL = "https://house.sjcnu.com/esp32/firmware/" OTA_VERSION "/" PROJECT_NAME ".bin";

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

static Program_Entry Programs[NUM_PROGRAMS] = {    
    {"Tester",    TesterProgramLines,      sizeof(TesterProgramLines) / sizeof(TesterProgramLines[0])},
    {"Normal",  NormalProgramLines,      sizeof(NormalProgramLines) / sizeof(NormalProgramLines[0])},    
    {"HiTemp", HiTempProgramLines,      sizeof(HiTempProgramLines) / sizeof(HiTempProgramLines[0])}};

#define setCharArray(target, value)                                            \
  do {                                                                         \
    strncpy((target), (value), sizeof(target) - 1);                            \
    (target)[sizeof(target) - 1] = '\0';                                       \
  } while (0)

// Print bits from "value" according to which positions are flagged in "mask"
static inline void print_masked_bits(uint64_t value, uint64_t mask) {
    for (int bit = 63; bit >= 0; bit--) {
        if (mask & (1ULL << bit)) {
            printf("%d", (int)((value >> bit) & 1ULL));
        }
    }
    printf("\n");
}


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
  char statusstring[512]; // OPTIMIZATION: Fixed size buffer
  char IPAddress[16];     // OPTIMIZATION: Fixed size for IP
  char Program[10];
  bool HEAT_REQUESTED;
} status_struct;
#endif