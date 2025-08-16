#ifndef ANALOG_H
#define ANALOG_H

void init_adc();
void sample_analog_inputs_task(void *arg);
float convert_adc_to_fahrenheit(int adc_val);
void temp_monitor_task(void *arg);

#endif
