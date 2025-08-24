// Minimal POST-only HTTP server:
// - /status returns JSON via chunked transfer; times are mm:ss
// - /action/* control endpoints (simple queue + single-run guard)
// - No big JSON buffers, no append helper

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
#include <strings.h>  // strncasecmp

#include "http_server.h"
#include "dishwasher_programs.h"   // extern status_struct ActiveStatus; run_program()
#include "local_ota.h"             // check_and_perform_ota()

#ifndef TAG
#define TAG "http_server"
#endif

// ===== Config =====
#define ACTION_QUEUE_LEN   16
#define ACTION_TASK_STACK  4096
#define ACTION_TASK_PRIO   5
#define RUN_PROGRAM_STACK  8192

// ===== Externals you already have =====


// ===== Devices (names for /status list) =====
typedef enum {
  DEVICE_HEAT = 0,
  DEVICE_SPRAY,
  DEVICE_FILL,
  DEVICE_DRAIN,
  DEVICE_SOAP,
  DEVICE_MAX
} device_t;

static const char *DEVICE_NAMES[DEVICE_MAX] = {
  "Heat","Spray","Fill","Drain","Soap"
};

// ===== Module state =====
static httpd_handle_t s_server       = NULL;
static QueueHandle_t  s_action_queue = NULL;
static TaskHandle_t   s_action_task  = NULL;
static TaskHandle_t   s_program_task = NULL; // single-run guard

// Sticky soap indicator for the current program
static bool    s_soap_dispensed_sticky = false;
static int64_t s_last_program_start_ms = -1;

// ===== Helpers =====
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

// mm:ss formatter; returns pointer to provided buffer
static const char* ms_to_mmss(int64_t ms, char out[8]) {
  if (ms < 0) { strcpy(out, "--:--"); return out; }
  int64_t secs = ms / 1000;
  int mm = (int)(secs / 60);
  int ss = (int)(secs % 60);
  snprintf(out, 8, "%02d:%02d", mm, ss);
  return out;
}

static void drain_body(httpd_req_t *req) {
  int remaining = req->content_len;
  while (remaining > 0) {
    char tmp[128];
    int r = httpd_req_recv(req, tmp, (remaining > (int)sizeof(tmp)) ? sizeof(tmp) : remaining);
    if (r <= 0) break;
    remaining -= r;
  }
}

static bool has_token_ci(const char *s, const char *token) {
  if (!s || !token) return false;
  size_t n = strlen(token);
  for (const char *p = s; *p; ++p) {
    if (strncasecmp(p, token, n) == 0) return true;
  }
  return false;
}

// Parse ActiveStatus.ActiveDevices into per-device booleans
// Accepts either words ("heat") or letter codes (H,S,F, D, P)
static void parse_active_devices(const char *src, bool on[DEVICE_MAX]) {
  for (int i = 0; i < DEVICE_MAX; i++) on[i] = false;
  if (!src) return;

  if (has_token_ci(src, "heat"))  on[DEVICE_HEAT]  = true;
  if (has_token_ci(src, "spray")) on[DEVICE_SPRAY] = true;
  if (has_token_ci(src, "fill"))  on[DEVICE_FILL]  = true;
  if (has_token_ci(src, "drain")) on[DEVICE_DRAIN] = true;
  if (has_token_ci(src, "soap"))  on[DEVICE_SOAP]  = true;

  for (const char *p = src; *p; ++p) {
    switch (*p) {
      case 'H': case 'h': on[DEVICE_HEAT]  = true; break;
      case 'S': case 's': on[DEVICE_SPRAY] = true; break;
      case 'F': case 'f': on[DEVICE_FILL]  = true; break;
      case 'D': case 'd': on[DEVICE_DRAIN] = true; break;
      case 'P': case 'p': on[DEVICE_SOAP]  = true; break;
      default: break;
    }
  }
}

// ===== /status (POST-only, chunked JSON, times = mm:ss) =====
static esp_err_t handle_status(httpd_req_t *req) {
  drain_body(req);

  // Prefer "full" timing when present
  int64_t start_ms = (ActiveStatus.time_full_start > 0)
                      ? ActiveStatus.time_full_start
                      : ActiveStatus.time_start;

  int64_t total_ms = (ActiveStatus.time_full_total > 0)
                      ? ActiveStatus.time_full_total
                      : ActiveStatus.time_total;

  // Detect program change to reset sticky soap
  if (start_ms > 0 && start_ms != s_last_program_start_ms) {
    s_last_program_start_ms = start_ms;
    s_soap_dispensed_sticky = false;
  }

  // Compute elapsed/remaining
  int64_t elapsed_ms = -1;
  if (ActiveStatus.time_elapsed >= 0) {
    elapsed_ms = ActiveStatus.time_elapsed;
  } else if (start_ms > 0) {
    elapsed_ms = now_ms() - start_ms;
    if (elapsed_ms < 0) elapsed_ms = 0;
  }

  int64_t remaining_ms = -1;
  if (ActiveStatus.time_total > 0 && ActiveStatus.time_elapsed >= 0) {
    remaining_ms = ActiveStatus.time_total - ActiveStatus.time_elapsed;
  } else if (start_ms > 0 && total_ms > 0) {
    remaining_ms = (start_ms + total_ms) - now_ms();
  }
  if (remaining_ms < 0) remaining_ms = 0;

  // "ETA" expressed as time-from-now (same units mm:ss)
  int64_t eta_from_now_ms = remaining_ms;

  bool on[DEVICE_MAX];
  parse_active_devices(ActiveStatus.ActiveDevices, on);

  // Make soap sticky if seen active or when step says "soap"
  if (on[DEVICE_SOAP] || has_token_ci(ActiveStatus.Step, "soap")) {
    s_soap_dispensed_sticky = true;
  }

  char num[64], mmss1[8], mmss2[8], mmss3[8];

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{");

  httpd_resp_sendstr_chunk(req, "\"Program\":\"");
  httpd_resp_sendstr_chunk(req, ActiveStatus.Program);
  httpd_resp_sendstr_chunk(req, "\",");

  httpd_resp_sendstr_chunk(req, "\"name_cycle\":\"");
  httpd_resp_sendstr_chunk(req, ActiveStatus.Cycle);
  httpd_resp_sendstr_chunk(req, "\",");

  httpd_resp_sendstr_chunk(req, "\"name_step\":\"");
  httpd_resp_sendstr_chunk(req, ActiveStatus.Step);
  httpd_resp_sendstr_chunk(req, "\",");

  snprintf(num, sizeof(num), "\"CurrentTemp\":%d,", ActiveStatus.CurrentTemp);
  httpd_resp_sendstr_chunk(req, num);

  // Times as mm:ss
  httpd_resp_sendstr_chunk(req, "\"since_start_mmss\":\"");
  httpd_resp_sendstr_chunk(req, ms_to_mmss(elapsed_ms, mmss1));
  httpd_resp_sendstr_chunk(req, "\",");

  httpd_resp_sendstr_chunk(req, "\"remaining_mmss\":\"");
  httpd_resp_sendstr_chunk(req, ms_to_mmss(remaining_ms, mmss2));
  httpd_resp_sendstr_chunk(req, "\",");

  httpd_resp_sendstr_chunk(req, "\"eta_finish_mmss\":\"");
  httpd_resp_sendstr_chunk(req, ms_to_mmss(eta_from_now_ms, mmss3));
  httpd_resp_sendstr_chunk(req, "\",");

  // active_devices
  httpd_resp_sendstr_chunk(req, "\"active_devices\":[");
  bool first = true;
  for (int d = 0; d < DEVICE_MAX; d++) {
    if (on[d]) {
      if (!first) httpd_resp_sendstr_chunk(req, ",");
      httpd_resp_sendstr_chunk(req, "\"");
      httpd_resp_sendstr_chunk(req, DEVICE_NAMES[d]);
      httpd_resp_sendstr_chunk(req, "\"");
      first = false;
    }
  }
  httpd_resp_sendstr_chunk(req, "],");

  httpd_resp_sendstr_chunk(req, "\"soap_has_dispensed\":");
  httpd_resp_sendstr_chunk(req, s_soap_dispensed_sticky ? "true" : "false");

  httpd_resp_sendstr_chunk(req, "}\n");
  httpd_resp_send_chunk(req, NULL, 0); // end chunked response
  return ESP_OK;
}

// ===== Actions (simple queue + worker) =====
static void assign_program(const char *name) {
  if (!name) return;
  strncpy(ActiveStatus.Program, name, sizeof(ActiveStatus.Program) - 1);
  ActiveStatus.Program[sizeof(ActiveStatus.Program) - 1] = '\0';
}

static bool start_program_if_idle(const char *program_name) {
  if (s_program_task && eTaskGetState(s_program_task) != eDeleted) {
    _LOG_W("run_program already active; ignoring duplicate START");
    return false;
  }
  if (program_name && program_name[0]) assign_program(program_name);
  if (xTaskCreate(run_program, "run_program", RUN_PROGRAM_STACK, NULL, ACTION_TASK_PRIO, &s_program_task) != pdPASS) {
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
      // Implement a cooperative cancel flag inside your program loop if desired.
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
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "OK\n");
  return ESP_OK;
}

static void action_worker(void *arg) {
  (void)arg;
  for (;;) {
    actions_t a;
    if (xQueueReceive(s_action_queue, &a, portMAX_DELAY) == pdTRUE) {
      perform_action(a);
    }
  }
}

static esp_err_t handle_start  (httpd_req_t *req){ return handle_action_common(req, ACTION_START);  }
static esp_err_t handle_cancel (httpd_req_t *req){ return handle_action_common(req, ACTION_CANCEL); }
static esp_err_t handle_hitemp (httpd_req_t *req){ return handle_action_common(req, ACTION_HITEMP); }
static esp_err_t handle_update (httpd_req_t *req){ return handle_action_common(req, ACTION_UPDATE); }
static esp_err_t handle_reboot (httpd_req_t *req){ return handle_action_common(req, ACTION_REBOOT); }

// ===== Server lifecycle & routing (POST-only) =====
static void register_uri_post(httpd_handle_t s, const char *uri, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t u = {
    .uri = uri,
    .method = HTTP_POST,
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
  register_uri_post(s_server, "/action/start",  handle_start);
  register_uri_post(s_server, "/action/cancel", handle_cancel);
  register_uri_post(s_server, "/action/hitemp", handle_hitemp);
  register_uri_post(s_server, "/action/update", handle_update);
  register_uri_post(s_server, "/action/reboot", handle_reboot);

  // Status endpoint (POST-only)
  register_uri_post(s_server, "/status", handle_status);


httpd_uri_t status_get = { .uri="/status", .method=HTTP_GET, .handler=handle_status, .user_ctx=NULL };
httpd_register_uri_handler(s_server, &status_get);











  started = true;
  _LOG_I("webserver started");
}

void stop_webserver(void) {
  if (s_server) {
    httpd_stop(s_server);
  }
  s_server = NULL;
  _LOG_I("webserver stopped");
}
