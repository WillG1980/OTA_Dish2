#ifndef BUTTONS_H
#define BUTTONS_H

#include "driver/gpio.h"

typedef struct {
  int State;
  gpio_num_t Pin;
  char name[25];
} button_t;

typedef struct {
  int State;
  gpio_num_t Pin;
  char name[25];
} led_t;

extern button_t buttons[];
void init_switchesandleds();
void monitor_task_button(void *arg);

#define BUTTON_PRESSED 1
#define BUTTON_RELEASED 0
#define BUTTON_OFF 0

#endif