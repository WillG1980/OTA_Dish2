// http_server.c — regenerated from build-248-ok with current actions and rules
// - Wildcard POST handler ("/action/*")
// - Grouped buttons by <GROUP> from ACTION_<GROUP>_<BUTTON>
// - perform_action_<BUTTON>() stubs (weak) executed in worker task
// - /status JSON uses full ActiveStatus; times as MM:SS; start/end as EST AM/PM
// - 95% status viewport; refresh 10s or 1s after click; button pushed glow 2s
// - Braces on all if/for; no word-wrapped code lines

#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dishwasher_programs.h"
#include "http_server.h"
#include "local_ota.h"

#ifndef TAG
#define TAG "http_server"
#endif

/* Non-ESP-IDF / non-stock project symbols this file relies on:
 *  - _LOG_I/_LOG_W/_LOG_E logging macros (project logging facility)
 *  - ActiveStatus (extern struct) from dishwasher_programs.h
 *  - check_and_perform_ota() from local_ota.h
 */

#define ACTION_QUEUE_LEN 16
#define ACTION_TASK_STACK 4096
#define ACTION_TASK_PRIO 5
#define RUN_PROGRAM_STACK 8192
#define EST_OFFSET_SECONDS (-5 * 3600)

static httpd_handle_t s_server = NULL;
static QueueHandle_t s_action_queue = NULL;
static TaskHandle_t s_action_task = NULL;
static TaskHandle_t s_program_task = NULL;

// Forward declarations
static void action_worker(void *arg);
static esp_err_t generic_action_handler(httpd_req_t *req);
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t handle_status(httpd_req_t *req);

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static inline unsigned queue_depth(void) {
  return s_action_queue ? (unsigned)uxQueueMessagesWaiting(s_action_queue) : 0u;
}

static void drain_body(httpd_req_t *req) {
  int remaining = req->content_len;
  char tmp[128];
  while (remaining > 0) {
    int r = httpd_req_recv(
        req, tmp, (remaining > (int)sizeof(tmp)) ? sizeof(tmp) : remaining);
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

static const char *ms_to_mmss(int64_t ms, char out[8]) {
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

static void format_est_time_ms(int64_t epoch_ms, char out[16]) {
  if (epoch_ms <= 0) {
    strcpy(out, "--:--");
    return;
  }
  time_t t = (time_t)(epoch_ms / 1000) + EST_OFFSET_SECONDS;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  int hh = tmv.tm_hour % 12;
  if (hh == 0) {
    hh = 12;
  }
  const char *ampm = (tmv.tm_hour >= 12) ? "PM" : "AM";
  snprintf(out, 16, "%02d:%02d %s", hh, tmv.tm_min, ampm);
}

// JSON helpers
static void json_prop_str(httpd_req_t *req, bool *first, const char *key,
                          const char *val) {
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

static void json_prop_int(httpd_req_t *req, bool *first, const char *key,
                          int val) {
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

static void json_prop_bool(httpd_req_t *req, bool *first, const char *key,
                           bool b) {
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

// Status handler using ActiveStatus (GET only)
static esp_err_t handle_status(httpd_req_t *req) {
  int64_t start_ms = (ActiveStatus.time_full_start > 0)
                         ? ActiveStatus.time_full_start
                         : ActiveStatus.time_start;
  int64_t total_ms = (ActiveStatus.time_full_total > 0)
                         ? ActiveStatus.time_full_total
                         : ActiveStatus.time_total;
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
  if (has_token_ci(ActiveStatus.ActiveDevices, "soap") ||
      has_token_ci(ActiveStatus.Step, "soap")) {
    soap_sticky = true;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{");
  bool first = true;
  json_prop_str(req, &first, "Program", ActiveStatus.Program);
  json_prop_str(req, &first, "name_cycle", ActiveStatus.Cycle);
  json_prop_str(req, &first, "name_step", ActiveStatus.Step);
  json_prop_int(req, &first, "CurrentTemp", ActiveStatus.CurrentTemp);
  char mm1[8], mm2[8], mm3[8], tstart[16], tend[16];
  json_prop_str(req, &first, "since_start_mmss", ms_to_mmss(elapsed_ms, mm1));
  json_prop_str(req, &first, "remaining_mmss", ms_to_mmss(remaining_ms, mm2));
  json_prop_str(req, &first, "eta_finish_mmss", ms_to_mmss(remaining_ms, mm3));
  format_est_time_ms(start_ms, tstart);
  format_est_time_ms((start_ms > 0 && total_ms > 0) ? (start_ms + total_ms) : 0,
                     tend);
  json_prop_str(req, &first, "start_time_est", tstart);
  json_prop_str(req, &first, "end_time_est", tend);
  json_prop_bool(req, &first, "soap_has_dispensed", soap_sticky);

  httpd_resp_sendstr_chunk(req, "}\n");
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// Program control helpers and stubs
static void set_program_name(const char *name) {
  if (!name) {
    return;
  }
  size_t n = strlen(name);
  if (n >= sizeof(ActiveStatus.Program)) {
    n = sizeof(ActiveStatus.Program) - 1;
  }
  memcpy(ActiveStatus.Program, name, n);
  ActiveStatus.Program[n] = '\0';
}

// Optional cooperative cancel hook — override elsewhere to signal your program
// loop to exit gracefully
__attribute__((weak)) void request_program_cancel(void) {
  _LOG_W("request_program_cancel(): weak stub; override to signal your program "
         "to stop");
}

static void run_program_trampoline(void *arg) {
  (void)arg;
  run_program(NULL);
  s_program_task = NULL;
  vTaskDelete(NULL);
}

static bool start_program_if_idle(const char *program_name) {
  if (s_program_task && eTaskGetState(s_program_task) != eDeleted) {
    _LOG_W("run_program already active; ignoring new start for %s",
           program_name ? program_name : "<null>");
    return false;
  }
  if (program_name) {
    set_program_name(program_name);
  }
  if (xTaskCreate(run_program_trampoline, "run_program", RUN_PROGRAM_STACK,
                  NULL, ACTION_TASK_PRIO, &s_program_task) != pdPASS) {
    _LOG_E("failed to create run_program task");
    s_program_task = NULL;
    return false;
  }
  return true;
}

static bool cancel_and_start_program(const char *program_name) {
  if (s_program_task && eTaskGetState(s_program_task) != eDeleted) {
    _LOG_I("cancel_and_start_program: requesting cancel of running program");
    request_program_cancel();
    TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(3000);
    for (;;) {
      eTaskState st = eTaskGetState(s_program_task);
      if (st == eDeleted) {
        break;
      }
      if ((xTaskGetTickCount() - start) > timeout) {
        _LOG_W("cancel_and_start_program: cancel timeout — force deleting "
               "program task");
        vTaskDelete(s_program_task);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_program_task = NULL;
  }
  if (program_name) {
    set_program_name(program_name);
  }
  return start_program_if_idle(program_name);
}

// perform_action_<BUTTON>() stubs — CYCLE actions start program (guarded);
// others log by default
__attribute__((weak)) void perform_action_NORMAL(void) {
  _LOG_I("Action NORMAL");
  (void)start_program_if_idle("Normal");
}
__attribute__((weak)) void perform_action_TESTER(void) {
  _LOG_I("Action TESTER");
  (void)start_program_if_idle("Tester");
}
__attribute__((weak)) void perform_action_HITEMP(void) {
  _LOG_I("Action HITEMP");
  (void)start_program_if_idle("HiTemp");
}
__attribute__((weak)) void perform_action_PAUSE(void) {
  _LOG_I("Action PAUSE");
}
__attribute__((weak)) void perform_action_DO_RESUME(void) {
  _LOG_I("Action DO_RESUME");
}
__attribute__((weak)) void perform_action_DRAIN(void) {
  _LOG_I("Action DRAIN");
}
__attribute__((weak)) void perform_action_FILL(void) { _LOG_I("Action FILL"); }
__attribute__((weak)) void perform_action_SPRAY(void) {
  _LOG_I("Action SPRAY");
}
__attribute__((weak)) void perform_action_HEAT(void) { _LOG_I("Action HEAT"); }
__attribute__((weak)) void perform_action_SOAP(void) { _LOG_I("Action SOAP"); }
__attribute__((weak)) void perform_action_LEDS(void) { 

    int DELAY = pdMS_TO_TICKS(5000);

  LED_Toggle("status_washing", LED_ON);  vTaskDelay(DELAY);  LED_Toggle("status_washing", LED_OFF);  
  LED_Toggle("status_sensing", LED_ON);  vTaskDelay(DELAY);  LED_Toggle("status_sensing", LED_OFF);  
  LED_Toggle("status_drying", LED_ON);  vTaskDelay(DELAY);  LED_Toggle("status_drying", LED_OFF);  
  LED_Toggle("status_clean", LED_ON);  vTaskDelay(DELAY);  LED_Toggle("status_clean", LED_OFF);  
  LED_Toggle("delay_1", LED_ON);  vTaskDelay(DELAY);  LED_Toggle("delay_1", LED_OFF);
  LED_Toggle("delay_3", LED_ON);  vTaskDelay(DELAY);  LED_Toggle("delay_3", LED_OFF);
  LED_Toggle("switch_4", LED_ON);  vTaskDelay(delay);  LED_Toggle("switch_4", LED_OFF);  
 }
__attribute__((weak)) void perform_action_CANCEL(void) {
  _LOG_I("Action CANCEL — stop current and start Cancel program");
  (void)cancel_and_start_program("Cancel");
}
__attribute__((weak)) void perform_action_FIRMWARE(void) {
  _LOG_I("Action FIRMWARE");
  check_and_perform_ota();
}
__attribute__((weak)) void perform_action_REBOOT(void) {
  _LOG_I("Action REBOOT");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
}
__attribute__((weak)) void perform_action_SKIP_STEP(void) {
  _LOG_I("Action SKIP_STEP");
}

// Dispatch table from enum to perform_action_<BUTTON>()
static void dispatch_action(actions_t a) {
  switch (a) {
  case ACTION_CYCLE_NORMAL:
    perform_action_NORMAL();
    break;
  case ACTION_CYCLE_TESTER:
    perform_action_TESTER();
    break;
  case ACTION_CYCLE_HITEMP:
    perform_action_HITEMP();
    break;
  case ACTION_DO_PAUSE:
    perform_action_PAUSE();
    break;
  case ACTION_DO_RESUME:
    perform_action_DO_RESUME();
    break;
  case ACTION_TOGGLE_DRAIN:
    perform_action_DRAIN();
    break;
  case ACTION_TOGGLE_FILL:
    perform_action_FILL();
    break;
  case ACTION_TOGGLE_SPRAY:
    perform_action_SPRAY();
    break;
  case ACTION_TOGGLE_HEAT:
    perform_action_HEAT();
    break;
  case ACTION_TOGGLE_SOAP:
    perform_action_SOAP();
    break;
  case ACTION_TOGGLE_LEDS:
    perform_action_LEDS();
    break;
  case ACTION_ADMIN_CANCEL:
    perform_action_CANCEL();
    break;
  case ACTION_ADMIN_FIRMWARE:
    perform_action_FIRMWARE();
    break;
  case ACTION_ADMIN_REBOOT:
    perform_action_REBOOT();
    break;
  case ACTION_ADMIN_SKIP_STEP:
    perform_action_SKIP_STEP();
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
      dispatch_action(a);
    }
  }
}

// Routes table used for UI grouping and wildcard matching
typedef struct {
  const char *group;
  const char *name;
  actions_t act;
} route_t;
static const route_t ROUTES[] = {
    {"CYCLE", "NORMAL", ACTION_CYCLE_NORMAL},
    {"CYCLE", "TESTER", ACTION_CYCLE_TESTER},
    {"CYCLE", "HITEMP", ACTION_CYCLE_HITEMP},
    {"DO", "PAUSE", ACTION_DO_PAUSE},
    {"DO", "RESUME", ACTION_DO_RESUME},
    {"TOGGLE", "DRAIN", ACTION_TOGGLE_DRAIN},
    {"TOGGLE", "FILL", ACTION_TOGGLE_FILL},
    {"TOGGLE", "SPRAY", ACTION_TOGGLE_SPRAY},
    {"TOGGLE", "HEAT", ACTION_TOGGLE_HEAT},
    {"TOGGLE", "SOAP", ACTION_TOGGLE_SOAP},
    {"TOGGLE", "LEDS", ACTION_TOGGLE_LEDS},
    {"ADMIN", "CANCEL", ACTION_ADMIN_CANCEL},
    {"ADMIN", "FIRMWARE", ACTION_ADMIN_FIRMWARE},
    {"ADMIN", "REBOOT", ACTION_ADMIN_REBOOT},
    {"ADMIN", "SKIP_STEP", ACTION_ADMIN_SKIP_STEP}};

// Wildcard POST /action/* → find GROUP/NAME in ROUTES
static esp_err_t generic_action_handler(httpd_req_t *req) {
  const char *uri = req->uri;
  if (strncmp(uri, "/action/", 8) != 0) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown action");
    return ESP_OK;
  }
  const char *p = uri + 8;
  const char *slash = strchr(p, '/');
  if (!slash) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad action");
    return ESP_OK;
  }
  size_t glen = (size_t)(slash - p);
  const char *name = slash + 1;
  for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); ++i) {
    if (strncmp(ROUTES[i].group, p, glen) == 0 &&
        ROUTES[i].group[glen] == '\0' && strcmp(ROUTES[i].name, name) == 0) {
      actions_t a = ROUTES[i].act;
      if (!s_action_queue) {
        httpd_resp_send_err(req, 503, "queue not ready");
        return ESP_OK;
      }
      if (xQueueSend(s_action_queue, &a, 0) != pdTRUE) {
        _LOG_W("action queue full; dropping %d", (int)a);
        httpd_resp_send_err(req, 503, "queue full");
        return ESP_OK;
      }
      unsigned depth = queue_depth();
      _LOG_I("action enqueued: %d (queue depth %u)", (int)a, depth); httpd_resp_sendstr(req, "OK\n"); return ESP_OK; } }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown action");
    return ESP_OK;
    }

    // Root UI with grouped buttons and 95% width status viewport
    static esp_err_t root_get_handler(httpd_req_t * req) {
      httpd_resp_set_type(req, "text/html");
      httpd_resp_sendstr_chunk(
          req,
          "<!doctype html><html><head><meta charset=\"utf-8\"><meta "
          "name=\"viewport\" content=\"width=device-width, "
          "initial-scale=1\"><title>Dishwasher</"
          "title><style>body{font-family:sans-serif;margin:1rem}.row{margin:0."
          "75rem 0}.btn{padding:0.6rem 1rem;margin:0.25rem;border:1px solid "
          "#ccc;border-radius:10px;cursor:pointer}.btn.pushed{background:#ddd}#"
          "status{width:95%;height:16rem;border:1px solid "
          "#ccc;padding:0.5rem;white-space:pre;overflow:auto}.group{font-"
          "weight:600;margin-right:0.5rem}</style></head><body>");
      const char *current_group = NULL;
      for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); ++i) {
        const route_t *r = &ROUTES[i];
        if (!current_group || strcmp(current_group, r->group) != 0) {
          if (current_group) {
            httpd_resp_sendstr_chunk(req, "</div>");
          }
          httpd_resp_sendstr_chunk(req,
                                   "<div class=\"row\"><span class=\"group\">");
          httpd_resp_sendstr_chunk(req, r->group);
          httpd_resp_sendstr_chunk(req, ":</span>");
          current_group = r->group;
        }
        httpd_resp_sendstr_chunk(req,
                                 "<button class=\"btn\" data-uri=\"/action/");
        httpd_resp_sendstr_chunk(req, r->group);
        httpd_resp_sendstr_chunk(req, "/");
        httpd_resp_sendstr_chunk(req, r->name);
        httpd_resp_sendstr_chunk(req, "\">");
        httpd_resp_sendstr_chunk(req, r->name);
        httpd_resp_sendstr_chunk(req, "</button>");
      }
      if (current_group) {
        httpd_resp_sendstr_chunk(req, "</div>");
      }
      httpd_resp_sendstr_chunk(
          req,
          "<h3>Status</h3><pre id=\"status\"></pre><script>const "
          "statusBox=document.getElementById('status');async function "
          "refresh(){try{const r=await fetch('/status');const t=await "
          "r.text();statusBox.textContent=t;}catch(e){statusBox.textContent='("
          "error fetching /status)'}}function "
          "pushMark(btn){btn.classList.add('pushed');setTimeout(()=>btn."
          "classList.remove('pushed'),2000)}async function "
          "fire(uri,btn){pushMark(btn);try{await "
          "fetch(uri,{method:'POST'});}catch(e){} "
          "setTimeout(refresh,1000);}document.querySelectorAll('.btn').forEach("
          "b=>b.addEventListener('click',()=>fire(b.dataset.uri,b)));"
          "setInterval(refresh,10000);refresh();</script></body></html>");
      return httpd_resp_sendstr_chunk(req, NULL);
    }

    void start_webserver(void) {
      if (s_server) {
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
        if (xTaskCreate(action_worker, "action_worker", ACTION_TASK_STACK, NULL,
                        ACTION_TASK_PRIO, &s_action_task) != pdPASS) {
          _LOG_E("failed to create action_worker");
          return;
        }
      }
      httpd_config_t config = HTTPD_DEFAULT_CONFIG();
      config.uri_match_fn = httpd_uri_match_wildcard;
      if (httpd_start(&s_server, &config) != ESP_OK) {
        _LOG_E("httpd_start failed");
        s_server = NULL;
        return;
      }
      // GET only for root and /status
      httpd_uri_t status_get = {.uri = "/status",
                                .method = HTTP_GET,
                                .handler = handle_status,
                                .user_ctx = NULL};
      httpd_register_uri_handler(s_server, &status_get);
      httpd_uri_t action_post = {.uri = "/action/*",
                                 .method = HTTP_POST,
                                 .handler = generic_action_handler,
                                 .user_ctx = NULL};
      httpd_register_uri_handler(s_server, &action_post);
      httpd_uri_t root_get = {.uri = "/",
                              .method = HTTP_GET,
                              .handler = root_get_handler,
                              .user_ctx = NULL};
      httpd_register_uri_handler(s_server, &root_get);
      _LOG_I("webserver started");
    }

    void stop_webserver(void) {
      if (s_server) {
        httpd_stop(s_server);
      }
      s_server = NULL;
    }
    bool http_server_is_running(void) { return s_server != NULL; }
