// http_server.h
#pragma once
#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_

#include <stdbool.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTP server.
 *
 * Safe to call multiple times: the implementation guards initialization
 * and will return the existing handle if already running.
 *
 * @return httpd_handle_t server handle on success, or NULL on failure.
 */
httpd_handle_t start_webserver(void);

/**
 * Stop the HTTP server if running.
 * Safe to call even if not running.
 */
void stop_webserver(void);

/**
 * Legacy compatibility: previously required explicit init.
 * Now initialization is handled inside start_webserver(), so this is a no-op.
 */
static inline void http_server_actions_init(void) { /* no-op */ }

#ifdef __cplusplus
} // extern "C"
#endif

#endif // HTTP_SERVER_H_
