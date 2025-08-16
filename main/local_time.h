#ifndef TIME_UTILS_H
#define TIME_UTILS_H
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif
void initialize_sntp_blocking(void);
time_t get_unix_epoch(void);
void print_us_time(time_t timestamp);
const char *get_us_time_string(time_t timestamp);

#ifdef __cplusplus
}
#endif

#endif // TIME_UTILS_H
