#include "analog.h"
#include "driver/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <limits.h>
#include <string.h>
#include "analog.h"
#include "buttons.h"
#include "dishwasher_programs.h"
#include "local_ota.h"   // <- use headers, not .c
#include "local_time.h"
#include "local_wifi.h"  // <- use headers, not .c


#define ADC_ATTEN ADC_ATTEN_DB_11
#define ADC_BIT_WIDTH ADC_BITWIDTH_12
#define ADC_CHANNEL_7 ADC_CHANNEL_7
#define LOW_LIMIT 140
#define _HIGH_LIMIT 150

static adc_oneshot_unit_handle_t adc1_handle;
static adc_oneshot_unit_init_cfg_t init_config = {
    .unit_id = ADC_UNIT_1,
};

void init_adc() {
  adc_oneshot_new_unit(&init_config, &adc1_handle);
  adc_oneshot_chan_cfg_t adc_channel_config = {
      .atten = ADC_ATTEN,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_7, &adc_channel_config);
}

float convert_adc_to_fahrenheit(int adc_val) {
  const float R_FIXED = 19700.0;
  const float VCC = 3.3;
  const int ADC_MAX = 4095;
  const float BETA = 4300.0;
  const float T0 = 322.04;
  const float R0 = 21500.0;

  float v_out = ((float)adc_val / ADC_MAX) * VCC;

  if (v_out <= 0.0 || v_out >= VCC)
    return -999.0;

  float r_therm = R_FIXED * (v_out / (VCC - v_out));
  float inv_T = (1.0 / T0) + (1.0 / BETA) * log(r_therm / R0);
  float temp_K = 1.0 / inv_T;

  return (temp_K - 273.15) * 9.0 / 5.0 + 32.0;
}

void temp_monitor_task(void *arg) {
  while (true) {
    if (ActiveStatus.HEAT_REQUESTED && (ActiveStatus.CurrentTemp < LOW_LIMIT || ActiveStatus.CurrentTemp > _HIGH_LIMIT)) {
      _LOG_W("Temp %d out of range (%d-%d). Toggling device %s",
             status.CurrentTemp, LOW_LIMIT, _HIGH_LIMIT, dev_name[0]);

      toggle_device(dev_name[0]);
    }
    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}

void sample_analog_inputs_task(void *arg) {
  while (true) {
    if (strcmp(status.ActiveState, "OFF") == 0) {
      _LOG_I("Dishwasher is OFF - not scanning");
      vTaskDelay(pdMS_TO_TICKS(60000));
    }

    int Sensor_EnableID = get_deviceid_by_name("Sensor_Enable");
    if (Sensor_EnableID < 0) {
      _LOG_E("Sensor_Enable device not found");
      vTaskDelay(pdMS_TO_TICKS(60000));
      continue;
    }

    if (!devices[Sensor_EnableID].state)
      toggle_device("Sensor_Enable");

    vTaskDelay(pdMS_TO_TICKS(1000));

    int samples = 50;
    int min_temp = INT_MAX, max_temp = 0, temp_sum = 0;

    for (int i = 0; i < samples; i++) {
      int temp_val = 0;
      adc_oneshot_read(adc1_handle, ADC_CHANNEL_7, &temp_val);
      min_temp = (temp_val < min_temp) ? temp_val : min_temp;
      max_temp = (temp_val > max_temp) ? temp_val : max_temp;
      temp_sum += temp_val;
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    status.CurrentTemp = convert_adc_to_fahrenheit(temp_sum / samples);
    float min_var = 100.0f * ((temp_sum / samples) - min_temp) / (temp_sum / samples);
    float max_var = 100.0f * (max_temp - (temp_sum / samples)) / (temp_sum / samples);

    _LOG_I("Avg Temp: %d, min: %d max: %d variance - %.2f + %.2f",
           status.CurrentTemp, min_temp, max_temp, min_var, max_var);

    toggle_device("Sensor_Enable");
    vTaskDelay(pdMS_TO_TICKS(19000));
  }
}
