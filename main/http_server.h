#pragma once
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time setup for the action queue and worker task.
   Safe to call multiple times; does nothing after the first. */
void http_server_actions_init(void);

/* Start/stop the HTTP server. start_webserver() is idempotent. */
httpd_handle_t start_webserver(void);
void stop_webserver(void);

#ifdef __cplusplus
}
#endif
