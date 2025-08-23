#pragma once
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start/stop the HTTP server. start_webserver() is idempotent. */
httpd_handle_t start_webserver(void);
void stop_webserver(void);

#ifdef __cplusplus
}
#endif
