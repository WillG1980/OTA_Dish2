#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Simple blocking GET. Writes up to out_len-1 bytes and NUL-terminates.
// Returns ESP_OK for 2xx status, ESP_FAIL otherwise.
char * _http_get(const char *url, int *out_len, int *status);
esp_err_t http_get(const char *url, char *out_buf, size_t out_len, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
