// http_server.c — grouped actions, root UI, and queue-depth logging
// Option B: single wildcard POST handler ("/action/*") to avoid per-action slots
// Uses your actions_t (CYCLE/DO/TOGGLE/ADMIN). Buttons grouped per GROUP.

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "http_server.h"
#include "dishwasher_programs.h"
#include "local_ota.h"

#ifndef TAG
#define TAG "http_server"
#endif

#define ACTION_QUEUE_LEN   16
#define ACTION_TASK_STACK  4096
#define ACTION_TASK_PRIO   5
#define RUN_PROGRAM_STACK  8192

static httpd_handle_t s_server       = NULL;
static QueueHandle_t  s_action_queue = NULL;
static TaskHandle_t   s_action_task  = NULL;
static TaskHandle_t   s_program_task = NULL;

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static inline unsigned queue_depth(void) { return s_action_queue ? (unsigned)uxQueueMessagesWaiting(s_action_queue) : 0u; }

static void drain_body(httpd_req_t *req) {
    int remaining = req->content_len;
    char tmp[128];
    while (remaining > 0) {
        int r = httpd_req_recv(req, tmp, (remaining > (int)sizeof(tmp)) ? sizeof(tmp) : remaining);
        if (r <= 0) {
            break;
        }
        remaining -= r;
    }
}

static bool has_token_ci(const char *s, const char *token) {
    if (!s || !token) {
        return false;
    }
    size_t n = strlen(token);
    for (const char *p = s; *p; ++p) {
        if (strncasecmp(p, token, n) == 0) {
            return true;
        }
    }
    return false;
}

static const char* ms_to_mmss(int64_t ms, char out[8]) {
    // Formats times as MM:SS (drops milliseconds)
    if (ms < 0) {
        strcpy(out, "--:--");
        return out;
    }
    int64_t secs = ms / 1000;
    int mm = (int)(secs / 60);
    int ss = (int)(secs % 60);
    snprintf(out, 8, "%02d:%02d", mm, ss);
    return out;
}

// JSON helpers to ensure valid formatting
static void json_prop_str(httpd_req_t *req, bool *first, const char *key, const char *val) {
    if (!*first) {
        httpd_resp_sendstr_chunk(req, ",");
    } else {
        *first = false;
    }
    httpd_resp_sendstr_chunk(req, "\"");
    httpd_resp_sendstr_chunk(req, key);
    httpd_resp_sendstr_chunk(req, "\":\"");
    httpd_resp_sendstr_chunk(req, val ? val : "");
    httpd_resp_sendstr_chunk(req, "\"");
}

static void json_prop_int(httpd_req_t *req, bool *first, const char *key, int val) {
    char num[24];
    snprintf(num, sizeof(num), "%d", val);
    if (!*first) {
        httpd_resp_sendstr_chunk(req, ",");
    } else {
        *first = false;
    }
    httpd_resp_sendstr_chunk(req, "\"");
    httpd_resp_sendstr_chunk(req, key);
    httpd_resp_sendstr_chunk(req, "\":");
    httpd_resp_sendstr_chunk(req, num);
}

static void json_prop_bool(httpd_req_t *req, bool *first, const char *key, bool b) {
    if (!*first) {
        httpd_resp_sendstr_chunk(req, ",");
    } else {
        *first = false;
    }
    httpd_resp_sendstr_chunk(req, "\"");
    httpd_resp_sendstr_chunk(req, key);
    httpd_resp_sendstr_chunk(req, "\":");
    httpd_resp_sendstr_chunk(req, b ? "true" : "false");
}

// ──────────────────────────────────────────────────────────────────────────────
// Status JSON (GET/POST)
// ──────────────────────────────────────────────────────────────────────────────
static esp_err_t handle_status(httpd_req_t *req) {
    drain_body(req);

    int64_t start_ms = (ActiveStatus.time_full_start > 0) ? ActiveStatus.time_full_start : ActiveStatus.time_start;
    int64_t total_ms = (ActiveStatus.time_full_total > 0) ? ActiveStatus.time_full_total : ActiveStatus.time_total;

    static bool soap_sticky = false;
    static int64_t last_prog_start = -1;
    if (start_ms > 0 && start_ms != last_prog_start) {
        last_prog_start = start_ms;
        soap_sticky = false;
    }

    int64_t elapsed_ms = -1;
    if (ActiveStatus.time_elapsed >= 0) {
        elapsed_ms = ActiveStatus.time_elapsed;
    } else if (start_ms > 0) {
        elapsed_ms = now_ms() - start_ms;
        if (elapsed_ms < 0) {
            elapsed_ms = 0;
        }
    }

    int64_t remaining_ms = -1;
    if (ActiveStatus.time_total > 0 && ActiveStatus.time_elapsed >= 0) {
        remaining_ms = ActiveStatus.time_total - ActiveStatus.time_elapsed;
    } else if (start_ms > 0 && total_ms > 0) {
        remaining_ms = (start_ms + total_ms) - now_ms();
    }
    if (remaining_ms < 0) {
        remaining_ms = 0;
    }

    if (has_token_ci(ActiveStatus.ActiveDevices, "soap") || has_token_ci(ActiveStatus.Step, "soap")) {
        soap_sticky = true;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{");
    bool first = true;

    json_prop_str(req, &first, "Program", ActiveStatus.Program);
    json_prop_str(req, &first, "name_cycle", ActiveStatus.Cycle);
    json_prop_str(req, &first, "name_step", ActiveStatus.Step);
    json_prop_int(req, &first, "CurrentTemp", ActiveStatus.CurrentTemp);

    char mm1[8], mm2[8], mm3[8];
    json_prop_str(req, &first, "since_start_mmss", ms_to_mmss(elapsed_ms, mm1));
    json_prop_str(req, &first, "remaining_mmss", ms_to_mmss(remaining_ms, mm2));
    json_prop_str(req, &first, "eta_finish_mmss", ms_to_mmss(remaining_ms, mm3));
    json_prop_bool(req, &first, "soap_has_dispensed", soap_sticky);

    httpd_resp_sendstr_chunk(req, "}\n");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
void start_webserver(void) {
    if (s_server) { return; }

    // Ensure the action queue/worker exist before starting HTTPD
    if (!s_action_queue) {
        s_action_queue = xQueueCreate(ACTION_QUEUE_LEN, sizeof(actions_t));
        if (!s_action_queue) { _LOG_E("failed to create action queue"); return; }
    }
    if (!s_action_task) {
        if (xTaskCreate(action_worker, "action_worker",
                        ACTION_TASK_STACK, NULL, ACTION_TASK_PRIO,
                        &s_action_task) != pdPASS) {
            _LOG_E("failed to create action_worker");
            return;
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;  // required for "/action/*"

    if (httpd_start(&s_server, &config) != ESP_OK) {
        _LOG_E("httpd_start failed");
        s_server = NULL;
        return;
    }

    // /status (GET + POST)
    httpd_uri_t status_get  = { .uri="/status", .method=HTTP_GET , .handler=handle_status, .user_ctx=NULL };
    httpd_uri_t status_post = { .uri="/status", .method=HTTP_POST, .handler=handle_status, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &status_get);
    httpd_register_uri_handler(s_server, &status_post);

    // Single wildcard route for all actions: /action/* (POST)
    httpd_uri_t action_post = { .uri="/action/*", .method=HTTP_POST, .handler=generic_action_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &action_post);

    // Root UI
    httpd_uri_t root_get = { .uri="/", .method=HTTP_GET, .handler=root_get_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &root_get);

    _LOG_I("webserver started");
}

void stop_webserver(void) {
    if (s_server) { httpd_stop(s_server); }
    s_server = NULL;
}

bool http_server_is_running(void) {
    return s_server != NULL;
}
