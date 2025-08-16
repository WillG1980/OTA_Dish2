<<<<<<< HEAD
// main.c
#ifndef PROJECT_NAME
#define PROJECT_NAME "OTA-Dishwasher"
#endif
#ifndef TAG
#define TAG PROJECT_NAME
#endif
#include "dishwasher_programs.h"
=======
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
#include "buttons.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define LED_GPIO GPIO_NUM_19
#define BUTTON_GPIO GPIO_NUM_16
#define GND_GPIO GPIO_NUM_17
extern const char *__TAG__;

button_t Buttons[]={
{false,GPIO_NUM_16,"Start"},
{false,GPIO_NUM_17,"Cancel"}
};
led_t Leds[]={
  {false,GPIO_NUM_18,"Clean Identifier"},
  {false,GPIO_NUM_19,"Status Identifier"}
};
void init_switchesandleds() {
<<<<<<< HEAD
  _LOG_I("Start of function");
=======
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
  for (int i = 0; i < sizeof(Buttons) / sizeof(Buttons[0]); i++) {
    gpio_config_t sw_conf = {
      .pin_bit_mask = (1ULL << Buttons[i].Pin),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&sw_conf);
  }

  for (int i = 0; i < sizeof(Leds) / sizeof(Leds[0]); i++) {
    gpio_config_t led_conf = {
      .pin_bit_mask = (1ULL << Leds[i].Pin),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_conf);
    gpio_set_level(Leds[i].Pin, 0); // Ensure LEDs start off
  }

  // Optional: configure a shared GND line if used
  gpio_config_t gnd_conf = {
    .pin_bit_mask = (1ULL << GND_GPIO),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  gpio_config(&gnd_conf);
<<<<<<< HEAD

  gpio_set_level(GND_GPIO, 0); // Provide GND ref
  _LOG_I("End of function");
=======
  gpio_set_level(GND_GPIO, 0); // Provide GND ref
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
}

void monitor_task_button(void *arg) {
  const TickType_t debounce_ticks = pdMS_TO_TICKS(50);
  int last_state = 1;
  TickType_t last_change_time = 0;

  while (true) {
    int current_state = gpio_get_level(Buttons[0].Pin);
    TickType_t current_time = xTaskGetTickCount();

    if (current_state != last_state) {
      if (current_time - last_change_time > debounce_ticks) {
        if (current_state == 0) {
          Buttons[0].State = true;
          gpio_set_level(Leds[0].Pin, 1);
        } else {
          Buttons[0].State = false;
          gpio_set_level(Leds[0].Pin, 0);
        }
        last_change_time = current_time;
      }
      last_state = current_state;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}