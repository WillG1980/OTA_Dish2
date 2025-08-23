// POST-only HTTP server with /action/* endpoints and a rich /status
// response sourced from ActiveStatus (dishwasher_programs.*).
//
// Drop-in replacement: if you already expose these endpoints, this file
// keeps them and only changes the /status payload/logic.
//
// Requires: ESP-IDF (esp_http_server), your existing _LOG_* macros,
// and the ActiveStatus struct from dishwasher_programs.h.
//
// Copyright ...

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdio.h>

#include "http_server.h"
#include "dishwasher_programs.h"   // for status_struct ActiveStatus, run_program(), etc.
#include "local_ota.h"             // for check_and_perform_ota()

// ---- Logging ----
#ifndef TAG
#define TAG "http_server"
#endif

// ---- Compile-time config ----
#define ACTION_QUEUE_LEN   16
#define ACTION_TASK_STACK  4096
#define ACTION_TASK_PRIO   5

// ---- Externals you already have in your project ----

// run_program task entry (provided by your program engine)

// Optional helper you may already have; if not, we set Program via strncpy.

// ---- Module state ----
static httpd_handle_t s_server       = NULL;
static QueueHandle_t  s_action_queue = NULL;
static TaskHandle_t   s_action_task  = NULL;
static TaskHandle_t   s_program_task = NULL;  // single-run guard for run_program

// For logging a readable snapshot of queued actions
typedef struct {
  actions_t action;
  int64_t   enqueued_ms;
} action_item_t;

static action_item_t s_qmirror[ACTION_QUEUE_LEN];
static size_t s_q_head = 0;
static size_t s_q_size = 0;

// ---- Utilities ----
static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static const char* action_name(actions_t a) {
  switch (a) {
    case ACTION_START:  return "START";
    case ACTION_CANCEL: return "CANCEL";
    case ACTION_HITEMP: return "HITEMP";
    case ACTION_UPDATE: return "UPDATE";
    case ACTION_REBOOT: return "REBOOT";
    default:            return "UNKNOWN";
  }
}

static void mirror_push(actions_t a) {
  if (s_q_size < ACTION_QUEUE_LEN) {
    size_t idx = (s_q_head + s_q_size) % ACTION_QUEUE_LEN;
    s_qmirror[idx].action = a;
    s_qmirror[idx].enqueued_ms = now_ms();
    s_q_size++;
  } else {
    s_qmirror[s_q_head].action = a;
    s_qmirror[s_q_head].enqueued_ms = now_ms();
    s_q_head = (s_q_head + 1) % ACTION_QUEUE_LEN;
  }
}

static void mirror_pop_front(void) {
  if (s_q_size > 0) {
    s_q_head = (s_q_head + 1) % ACTION_QUEUE_LEN;
    s_q_size--;
  }
}

static void log_queue_snapshot(void) {
  if (s_q_size == 0) {
    _LOG_D("queue: [empty]");
    return;
  }
  char buf[256]; buf[0] = 0;
  size_t pos = 0, cap = sizeof(buf);
  for (size_t i = 0; i < s_q_size; i++) {
    size_t idx = (s_q_head + i) % ACTION_QUEUE_LEN;
    const char *name = action_name(s_qmirror[idx].action);
    int n = snprintf(buf + pos, (pos < cap) ? cap - pos : 0, "%s%s", name,
                     (i + 1 < s_q_size) ? " -> " : "");
    if (n < 0) break;
    pos += (size_t)n;
    if (pos >= cap) break;
  }
  _LOG_D("queue snapshot: %s", buf[0] ? buf : "(overflow)");
}

// ---- POST body drain (tidy up even if unused) ----
static void drain_body(httpd_req_t *req) {
  int remaining = req->content_len;
  while (remaining > 0) {
    char tmp[128];
    int r = httpd_req_recv(req, tmp, (remaining > (int)sizeof(tmp)) ? sizeof(tmp) : remaining);
    if (r <= 0) break;
    remaining -= r;
  }
}

// ===================================================
// /status  (UPDATED)
// ===================================================
//
// Returns JSON with:
// Program, name_cycle, name_step, CurrentTemp,
// start_time_epoch_ms, eta_finish_epoch_ms, remaining_ms,
// active_devices [Heat,Spray,Fill,Drain,Soap],
// soap_has_dispensed (sticky true for current program).
//
// ActiveStatus reference (from user):
// typedef struct {
//   int CurrentTemp;
//   int CurrentPower;
//   int64_t time_full_start;
//   int64_t time_full_total;
//   int64_t time_cycle_start;
//   int64_t time_cycle_total;
//   int64_t time_total;
//   int64_t time_elapsed;
//   int64_t time_start;
//   char Cycle[10];
//   char Step[10];
//   char statusstring[512];
//   char IPAddress[16];
//   char FirmwareStatus[20];
//   char Program[10];
//   bool HEAT_REQUESTED;
//   char ActiveDevices[10];
//   Program_Entry Active_Program;
// } status_struct;

static bool   s_soap_dispensed_sticky = false;
static int64_t s_last_program_start_ms = -1;

static bool has_token_ci(const char *s, const char *token) {
  if (!s || !token) return false;
  size_t n = strlen(token);
  for (const char *p = s; *p; ++p) {
    if (strncasecmp(p, token, n) == 0) return true;
  }
  return false;
}

static void parse_active_devices(const char *src,
                                 bool *heat, bool *spray, bool *fill, bool *drain, bool *soap)
{
  *heat = *spray = *fill = *drain = *soap = false;
  if (!src) return;

  // Named matches (case-insensitive)
  if (has_token_ci(src, "heat"))  *heat  = true;
  if (has_token_ci(src, "spray")) *spray = true;
  if (has_token_ci(src, "fill"))  *fill  = true;
  if (has_token_ci(src, "drain")) *drain = true;
  if (has_token_ci(src, "soap"))  *soap  = true;

  // Letter flag fallback (H,S,F,D,P)
  for (const char *p = src; *p; ++p) {
    switch (*p) {
      case 'H': case 'h': *heat  = true; break;
      case 'S': case 's': *spray = true; break;
      case 'F': case 'f': *fill  = true; break;
      case 'D': case 'd': *drain = true; break;
      case 'P': case 'p': *soap  = true; break;
      default: break;
    }
  }
}

static esp_err_t handle_status(httpd_req_t *req) {
  drain_body(req);

  // Choose program start & total; prefer "full" values when present
  int64_t start_ms = (ActiveStatus.time_full_start > 0)
                      ? ActiveStatus.time_full_start
                      : ActiveStatus.time_start;

  int64_t total_ms = (ActiveStatus.time_full_total > 0)
                      ? ActiveStatus.time_full_total
                      : ActiveStatus.time_total;

  // Reset sticky soap if program start changed
  if (start_ms > 0 && start_ms != s_last_program_start_ms) {
    s_last_program_start_ms = start_ms;
    s_soap_dispensed_sticky = false;
  }

  // Remaining time:
  // Prefer: time_total - time_elapsed (if populated)
  // Else:   (start + total) - now
  int64_t remaining_ms = -1;
  if (ActiveStatus.time_total > 0 && ActiveStatus.time_elapsed >= 0) {
    remaining_ms = ActiveStatus.time_total - ActiveStatus.time_elapsed;
    if (remaining_ms < 0) remaining_ms = 0;
  } else if (start_ms > 0 && total_ms > 0) {
    remaining_ms = (start_ms + total_ms) - now_ms();
    if (remaining_ms < 0) remaining_ms = 0;
  }

  int64_t eta_ms = -1;
  if (start_ms > 0 && total_ms > 0) {
    eta_ms = start_ms + total_ms;
  } else if (remaining_ms >= 0) {
    eta_ms = now_ms() + remaining_ms;
  }

  // Active devices → list
  bool heat=false, spray=false, fill=false, drain=false, soap=false;
  parse_active_devices(ActiveStatus.ActiveDevices, &heat, &spray, &fill, &drain, &soap);

  // Make soap sticky for the current program if observed active or step == "Soap"
  if (soap || has_token_ci(ActiveStatus.Step, "soap")) {
    s_soap_dispensed_sticky = true;
  }

  // Build JSON
  char json[1024];
  size_t pos = 0;
  #define APPEND(fmt, ...) do { \
    int n = snprintf(json + pos, (pos < sizeof(json)) ? sizeof(json) - pos : 0, fmt, __VA_ARGS__); \
    if (n < 0) n = 0; pos += (size_t)n; \
  } while(0)

  httpd_resp_set_type(req, "application/json");
  APPEND("{");
  APPEND("\"Program\":\"%s\",", ActiveStatus.Program);
  APPEND("\"name_cycle\":\"%s\",", ActiveStatus.Cycle);
  APPEND("\"name_step\":\"%s\",",  ActiveStatus.Step);
  APPEND("\"CurrentTemp\":%d,",    ActiveStatus.CurrentTemp);
  APPEND("\"start_time_epoch_ms\":%lld,", (long long)(start_ms > 0 ? start_ms : -1));
  APPEND("\"eta_finish_epoch_ms\":%lld,", (long long)(eta_ms   > 0 ? eta_ms   : -1));
  APPEND("\"remaining_ms\":%lld,",        (long long)(remaining_ms >= 0 ? remaining_ms : -1));
  APPEND("\"active_devices\":[");
    bool first = true;
    if (heat)  { APPEND("%s\"Heat\"",  first ? "" : ","); first = false; }
    if (spray) { APPEND("%s\"Spray\"", first ? "" : ","); first = false; }
    if (fill)  { APPEND("%s\"Fill\"",  first ? "" : ","); first = false; }
    if (drain) { APPEND("%s\"Drain\"", first ? "" : ","); first = false; }
    if (soap)  { APPEND("%s\"Soap\"",  first ? "" : ","); first = false; }
  APPEND("],");
  APPEND("\"soap_has_dispensed\":%s", s_soap_dispensed_sticky ? "true" : "false");
  APPEND("}\n");

  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ===================================================
// Action worker (unchanged semantics; POST-only handlers)
// ===================================================

static bool start_program_if_idle(const char *program_name) {
  if (s_program_task && eTaskGetState(s_program_task) != eDeleted) {
    _LOG_W("run_program already active; ignoring duplicate START");
    return false;
  }
  if (program_name && program_name[0]) {
    if (setCharArray) {
      setCharArray(ActiveStatus.Program, program_name);
    } else {
      strncpy(ActiveStatus.Program, program_name, sizeof(ActiveStatus.Program)-1);
      ActiveStatus.Program[sizeof(ActiveStatus.Program)-1] = '\0';
    }
  }
  if (xTaskCreate(run_program, "run_program", 8192, NULL, ACTION_TASK_PRIO, &s_program_task) != pdPASS) {
    _LOG_E("failed to create run_program task");
    s_program_task = NULL;
    return false;
  }
  return true;
}

static void perform_action(actions_t a) {
  switch (a) {
    case ACTION_START:
      _LOG_I("Performing START");
      (void)start_program_if_idle("Normal");
      break;
    case ACTION_HITEMP:
      _LOG_I("Performing HITEMP");
      (void)start_program_if_idle("HiTemp");
      break;
    case ACTION_CANCEL:
      _LOG_I("Performing CANCEL");
      // Prefer a cooperative cancel flag in your program loop.
      // Example: ActiveStatus.HEAT_REQUESTED = false;  // or set a Cancel flag you maintain
      break;
    case ACTION_UPDATE:
      _LOG_I("Performing UPDATE (OTA)");
      check_and_perform_ota();
      break;
    case ACTION_REBOOT:
      _LOG_I("Performing REBOOT");
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
      break;
    default:
      _LOG_W("Unknown action: %d", (int)a);
      break;
  }
}

static void action_worker(void *arg) {
  (void)arg;
  for (;;) {
    actions_t a;
    if (xQueueReceive(s_action_queue, &a, portMAX_DELAY) == pdTRUE) {
      mirror_pop_front();
      log_queue_snapshot();
      perform_action(a);
    }
  }
}

static esp_err_t handle_action_common(httpd_req_t *req, actions_t a) {
  drain_body(req);
  if (!s_action_queue) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "queue not ready\n");
    return ESP_OK;
  }
  if (xQueueSend(s_action_queue, &a, 0) != pdTRUE) {
    _LOG_W("action queue full; dropping %s", action_name(a));
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "queue full\n");
    return ESP_OK;
  }
  mirror_push(a);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "OK\n");
  return ESP_OK;
}

static esp_err_t handle_start  (httpd_req_t *req){ return handle_action_common(req, ACTION_START);  }
static esp_err_t handle_cancel (httpd_req_t *req){ return handle_action_common(req, ACTION_CANCEL); }
static esp_err_t handle_hitemp (httpd_req_t *req){ return handle_action_common(req, ACTION_HITEMP); }
static esp_err_t handle_update (httpd_req_t *req){ return handle_action_common(req, ACTION_UPDATE); }
static esp_err_t handle_reboot (httpd_req_t *req){ return handle_action_common(req, ACTION_REBOOT); }

// ===================================================
// Server lifecycle & routing (POST-only)
// ===================================================

static void register_uri(httpd_handle_t s, const char *uri, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t u = {
    .uri = uri,
    .method = HTTP_POST,   // POST-only as requested
    .handler = handler,
    .user_ctx = NULL
  };
  httpd_register_uri_handler(s, &u);
}

void start_webserver(void) {
  static bool started = false;
  if (started && s_server) {
    _LOG_I("webserver already started");
    return;
  }

  if (!s_action_queue) {
    s_action_queue = xQueueCreate(ACTION_QUEUE_LEN, sizeof(actions_t));
    if (!s_action_queue) {
      _LOG_E("failed to create action queue");
      return;
    }
  }

  if (!s_action_task) {
    if (xTaskCreate(action_worker, "action_worker", ACTION_TASK_STACK, NULL, ACTION_TASK_PRIO, &s_action_task) != pdPASS) {
      _LOG_E("failed to create action_worker");
      return;
    }
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    _LOG_E("httpd_start failed: %d", (int)err);
    return;
  }

  // Control endpoints (POST-only)
  register_uri(s_server, "/action/start",  handle_start);
  register_uri(s_server, "/action/cancel", handle_cancel);
  register_uri(s_server, "/action/hitemp", handle_hitemp);
  register_uri(s_server, "/action/update", handle_update);
  register_uri(s_server, "/action/reboot", handle_reboot);

  // Status endpoint (POST-only) — UPDATED
  register_uri(s_server, "/status", handle_status);

  started = true;
  _LOG_I("webserver started");
}

void stop_webserver(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    _LOG_I("webserver stopped");
  }
}
