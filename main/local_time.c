//#include "time_utils.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "dishwasher_programs.h"

#ifndef PROJECT_NAME
 #define PROJECT_NAME "OTA-Dishwasher"
#endif
<<<<<<< HEAD

#ifndef TAG
#define TAG PROJECT_NAME
#endif


void initialize_sntp_blocking(void)
{
    _LOG_I("Initializing SNTP");
=======
#ifndef __TAG__
#define __TAG__ PROJECT_NAME
#endif

static const char *TAG = PROJECT_NAME;

void initialize_sntp_blocking(void)
{
    ESP_LOGI(__TAG__, "Initializing SNTP");
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // Wait up to 30 seconds for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int max_retries = 30;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < max_retries) {
<<<<<<< HEAD
        _LOG_I("Waiting for system time to be set... (%d/%d)", retry, max_retries);
=======
        ESP_LOGI(__TAG__, "Waiting for system time to be set... (%d/%d)", retry, max_retries);
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year >= (2016 - 1900)) {
<<<<<<< HEAD
        _LOG_I("Time synchronized successfully.");
    } else {
        _LOG_W("Time synchronization failed after %d seconds.", max_retries);
=======
        ESP_LOGI(__TAG__, "Time synchronized successfully.");
    } else {
        ESP_LOGW(__TAG__, "Time synchronization failed after %d seconds.", max_retries);
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
    }
}

time_t get_unix_epoch(void)
{
    time_t now;
    time(&now);
    return now;
}

<<<<<<< HEAD
const char *get_us_time_string(time_t timestamp)
{
    static char time_str[80]; // Enough for full date+time+AM/PM
    struct tm timeinfo;

    // If timestamp is 0, use current time
    if (timestamp == 0) {
        time(&timestamp);
    }

    localtime_r(&timestamp, &timeinfo);

    // If time not set yet (esp-idf default year is 1970)
    if (timeinfo.tm_year < (2016 - 1900)) {
        _LOG_W("Time not set. Call initialize_sntp_blocking() first.");
        snprintf(time_str, sizeof(time_str), "TIME NOT SET");
        return time_str;
    }

    // Determine AM/PM and convert hour
=======
void print_us_time(void)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGW(__TAG__, "Time not set. Call initialize_sntp_blocking() first.");
        return;
    }

>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
    char am_pm[3];
    int hour = timeinfo.tm_hour;
    if (hour >= 12) {
        strcpy(am_pm, "PM");
        if (hour > 12) hour -= 12;
    } else {
        strcpy(am_pm, "AM");
        if (hour == 0) hour = 12;
    }

<<<<<<< HEAD
    snprintf(time_str, sizeof(time_str),
             "%02d/%02d/%04d %02d:%02d:%02d %.2s",
=======
    ESP_LOGI(__TAG__, "Current time: %02d/%02d/%04d %02d:%02d:%02d %s",
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday,
             timeinfo.tm_year + 1900,
             hour,
             timeinfo.tm_min,
             timeinfo.tm_sec,
             am_pm);
<<<<<<< HEAD

    return time_str;
=======
>>>>>>> ecef6b0d4d9f7b4ad0e2dea07c6d2b948dae4cbd
}