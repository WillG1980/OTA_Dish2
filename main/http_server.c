// http_server.c — POST-only, ACTION_MAX + queue snapshot logging
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h> // strcasecmp
#include <dishwasher_programs.h>

// Project headers (adjust paths if needed)
#include "dishwasher_programs.h" // run_program(), ActiveStatus, setCharArray(...)
#include "http_server.h"
#include "local_ota.h" // check_and_perform_ota()


// Use your project's logging macros
// Expecting _LOG_X(TAG, ...) to be defined by your project (INFO/DEBUG/etc).

/* ===================== Actions ===================== */

typedef enum {
  ACTION_NONE = 0,
  ACTION_START,
  ACTION_TEST,
  ACTION_HITEMP,
  ACTION_CANCEL,
  ACTION_UPDATE,
  ACTION_REBOOT,
  ACTION_MAX
} actions_t;

typedef void (*action_fn_t)(void);

typedef struct {
  const char *name; // API/UI name (case-insensitive match)
  action_fn_t fn;   // Work to perform on dequeue
  bool show_button; // Whether to render a button on "/"
} action_desc_t;

/* -------- Only-one-program guard -------- */
static TaskHandle_t program_task_handle = NULL;

static void program_task_trampoline(void *arg) {
  run_program(arg);
  program_task_handle = NULL; // clear singleton when the program ends
  vTaskDelete(NULL);
}

static void start_program_if_idle(const char *program_name) {
  if (program_task_handle != NULL) {
    _LOG_D(TAG, "Program already running; ignoring request");
    return;
  }
  setCharArray(ActiveStatus.Program, program_name);
  BaseType_t ok = xTaskCreate(program_task_trampoline, "run_program", 8192,
                              NULL, 5, &program_task_handle);
  if (ok != pdPASS) {
    program_task_handle = NULL;
    _LOG_E(TAG, "Failed to create run_program task");
  } else {
    _LOG_I(TAG, "Started program '%s'", program_name);
  }
}

/* -------- Concrete action functions -------- */
static void act_start(void) { start_program_if_idle("Normal"); }
static void act_test(void) { start_program_if_idle("Tester"); }
static void act_hitemp(void) { start_program_if_idle("HiTemp"); }
static void act_cancel(void) {
  _LOG_I(TAG, "Cancel requested"); /* hook your cancel here */
}
static void act_update(void) {
  _LOG_I(TAG, "Update requested");
  check_and_perform_ota();
}
static void act_reboot(void) {
  _LOG_I(TAG, "Reboot requested");
  esp_restart();
}

/* -------- Action table -------- */
static const action_desc_t ACTIONS[ACTION_MAX] = {
    [ACTION_NONE] = {.name = "None", .fn = NULL, .show_button = false},
    [ACTION_START] = {.name = "Start", .fn = act_start, .show_button = true},
    [ACTION_TEST] = {.name = "Test", .fn = act_test, .show_button = true},
    [ACTION_HITEMP] = {.name = "HiTemp", .fn = act_hitemp, .show_button = true},
    [ACTION_CANCEL] = {.name = "Cancel", .fn = act_cancel, .show_button = true},
    [ACTION_UPDATE] = {.name = "Update", .fn = act_update, .show_button = true},
    [ACTION_REBOOT] = {.name = "Reboot", .fn = act_reboot, .show_button = true},
};

static actions_t action_from_name(const char *s) {
  if (!s || !*s)
    return ACTION_NONE;
  for (int i = 1; i < ACTION_MAX; i++) {
    if (ACTIONS[i].name && strcasecmp(s, ACTIONS[i].name) == 0)
      return (actions_t)i;
  }
  return ACTION_NONE;
}

/* ===================== Queue/worker ===================== */

#define ACTION_Q_LEN 16

static QueueHandle_t action_queue = NULL;
static TaskHandle_t action_task_handle = NULL;

/* Build a one-line snapshot of the queue contents (front -> back) into buf. */
static void build_queue_snapshot(char *buf, size_t buf_sz) {
  if (!action_queue) {
    snprintf(buf, buf_sz, "(n/a)");
    return;
  }

  UBaseType_t pending_before = uxQueueMessagesWaiting(action_queue);
  if (pending_before == 0) {
    snprintf(buf, buf_sz, "(empty)");
    return;
  }

  actions_t tmp[ACTION_Q_LEN];
  UBaseType_t n = pending_before;
  if (n > ACTION_Q_LEN)
    n = ACTION_Q_LEN;

  /* Suspend scheduler to avoid race while we drain/restore with zero-timeout
   * ops. */
  vTaskSuspendAll();

  UBaseType_t got = 0;
  for (; got < n; ++got) {
    if (xQueueReceive(action_queue, &tmp[got], 0) != pdPASS)
      break;
  }

  /* Restore in same order so the queue content is unchanged for the next
   * operations. */
  for (UBaseType_t i = 0; i < got; ++i) {
    (void)xQueueSend(action_queue, &tmp[i], 0);
  }

  (void)xTaskResumeAll();

  /* Render: next-up -> ... -> last-in-queue */
  size_t w = 0;
  for (UBaseType_t i = 0; i < got; ++i) {
    const char *name =
        (tmp[i] > 0 && tmp[i] < ACTION_MAX && ACTIONS[tmp[i]].name)
            ? ACTIONS[tmp[i]].name
            : "?";
    int wrote = snprintf(buf + w, (w < buf_sz) ? (buf_sz - w) : 0, "%s%s", name,
                         (i + 1 < got) ? " -> " : "");
    if (wrote < 0)
      break;
    w += (size_t)wrote;
    if (w >= buf_sz)
      break;
  }

  if (got == 0) {
    snprintf(buf, buf_sz, "(empty)");
  }
}

static void action_worker_task(void *arg) {
  actions_t act;
  for (;;) {
    if (xQueueReceive(action_queue, &act, portMAX_DELAY) == pdPASS) {
      /* Log queue snapshot at the moment we are about to handle 'act' */
      char snap[256];
      build_queue_snapshot(snap, sizeof(snap));
      _LOG_D(TAG, "queue next-up -> last: %s", snap);

      _LOG_I(TAG, "Action dequeued=%d (%s)", (int)act,
             (act > 0 && act < ACTION_MAX && ACTIONS[act].name)
                 ? ACTIONS[act].name
                 : "?");

      if (act > 0 && act < ACTION_MAX && ACTIONS[act].fn) {
        ACTIONS[act].fn();
      } else {
        _LOG_W(TAG, "No handler for action=%d", (int)act);
      }
    }
  }
}

/* ===================== HTTP helpers ===================== */

static void set_common_headers(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",
                     "*"); // narrow if needed
}

/* Parse small POST body as form or minimal JSON to extract "action" */
static actions_t parse_action_from_body(httpd_req_t *req) {
  int total = req->content_len;
  if (total <= 0 || total > 256)
    return ACTION_NONE;

  char buf[257];
  int recvd = httpd_req_recv(req, buf, total);
  if (recvd <= 0)
    return ACTION_NONE;
  buf[recvd] = '\0';

  // form: action=Start
  const char *p = strstr(buf, "action=");
  if (p) {
    p += 7;
    char val[32] = {0};
    int i = 0;
    while (*p && *p != '&' && i < (int)sizeof(val) - 1)
      val[i++] = *p++;
    actions_t a = action_from_name(val);
    if (a != ACTION_NONE)
      return a;
  }

  // minimal JSON: {"action":"Start"}
  const char *q = strstr(buf, "\"action\"");
  if (q) {
    q = strchr(q, ':');
    if (q) {
      q++;
      while (*q == ' ' || *q == '\t')
        q++;
      if (*q == '"' || *q == '\'') {
        char quote = *q++;
        char val[32] = {0};
        int i = 0;
        while (*q && *q != quote && i < (int)sizeof(val) - 1)
          val[i++] = *q++;
        return action_from_name(val);
      }
    }
  }
  return ACTION_NONE;
}

/* ===================== HTTP handlers ===================== */
/* Root page: buttons + 95% width status window
   - POST-only UI
   - Auto-refresh /status every 10s
   - Also refresh /status 1s after any button click
*/
static esp_err_t action_post_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");

  actions_t act = parse_action_from_body(req);
  if (act == ACTION_NONE) {
    set_last_action_msg("invalid action (missing/unknown)");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "missing or invalid action");
    return ESP_OK;
  }

  if (!action_queue || xQueueSend(action_queue, &act, 0) != pdPASS) {
    UBaseType_t depth = action_queue ? uxQueueMessagesWaiting(action_queue) : 0;
    set_last_action_msg("queue busy (depth=%u)", (unsigned)depth);
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "busy");
    return ESP_OK;
  }

  UBaseType_t pending = uxQueueMessagesWaiting(action_queue);
  char msg[96];
  snprintf(msg, sizeof(msg), "queued %s; queue depth=%u\n",
           ACTIONS[act].name ? ACTIONS[act].name : "?",
           (unsigned)pending);

  set_last_action_msg("%s", msg);   // make it visible to /status
  httpd_resp_sendstr(req, msg);
  return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");

  httpd_resp_sendstr_chunk(
      req,
      "<!doctype html>"
      "<meta name=viewport content='width=device-width, initial-scale=1'>"
      "<title>Dishwasher</title>"
      "<style>"
      "button{margin:4px;padding:10px 16px;font-size:16px}"
      "#out{white-space:pre-wrap}"
      "#statusBox{width:95%;margin:12px auto;padding:10px;border:1px solid "
      "#ccc;"
      "border-radius:6px;min-height:140px;overflow:auto;background:#fafafa}"
      "</style>"
      "<h1>Dishwasher Controls</h1><div>");

  for (int i = 1; i < ACTION_MAX; i++) {
    if (ACTIONS[i].show_button && ACTIONS[i].name) {
      httpd_resp_sendstr_chunk(req, "<button onclick='doPost(\"");
      httpd_resp_sendstr_chunk(req, ACTIONS[i].name);
      httpd_resp_sendstr_chunk(req, "\")'>");
      httpd_resp_sendstr_chunk(req, ACTIONS[i].name);
      httpd_resp_sendstr_chunk(req, "</button>");
    }
  }

  httpd_resp_sendstr_chunk(
      req,
      "</div>"
      "<pre id='out'></pre>"
      "<h2>Status</h2>"
      "<div id='statusBox'><!-- status loads here --></div>"

      "<script>"
      "let statusTimer=null;"
      "async function loadStatus(){"
      "try{"
      "const r=await "
      "fetch('/status',{cache:'no-store',credentials:'same-origin'});"
      "const t=await r.text();"
      "document.getElementById('statusBox').innerHTML=t;"
      "}catch(e){"
      "document.getElementById('statusBox').textContent='Error loading "
      "/status: '+e;"
      "}"
      "}"
      "function startStatusAutoRefresh(periodMs){"
      "if(statusTimer) clearInterval(statusTimer);"
      "statusTimer=setInterval(loadStatus, periodMs);"
      "}"
      "async function doPost(action){"
      "const r=await "
      "fetch('/action',{method:'POST',headers:{'Content-Type':'application/"
      "x-www-form-urlencoded'},"
      "body:'action='+encodeURIComponent(action),cache:'no-store',credentials:'"
      "same-origin'});"
      "const t=await r.text();"
      "document.getElementById('out').textContent=(r.ok?'OK ':'ERR ')+t;"
      "setTimeout(loadStatus, 1000);" /* refresh /status 1s after click */
      "}"
      "window.addEventListener('load',()=>{"
      "loadStatus();"                  /* initial status load */
      "startStatusAutoRefresh(10000);" /* refresh every 10s */
      "});"
      "</script>");

  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

/* OPTIONS /action: CORS preflight */

static esp_err_t action_options_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",
                     "*"); // restrict if needed
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_sendstr(req, "");
  return ESP_OK;
}

/* Minimal status page */
static esp_err_t status_get_handler(httpd_req_t *req) {
  
  char line[192];
  analog_get_last_status(line, sizeof(line));

  set_common_headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(req, "<h1>Status</h1><pre>Program: ");
  httpd_resp_sendstr_chunk(req, ActiveStatus.Program);

  httpd_resp_sendstr_chunk(req, "<h2>Analog</h2><pre>");
  httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "</pre>");

  httpd_resp_sendstr_chunk(req, "\n</pre>");
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

/* ===================== Server lifecycle (guarded init) =====================
 */

static httpd_handle_t s_server = NULL;
static bool s_initialized = false;

httpd_handle_t start_webserver(void) {
  // One-time init (idempotent across multiple calls)
  if (!s_initialized) {
    s_initialized = true;
    action_queue = xQueueCreate(ACTION_Q_LEN, sizeof(actions_t));
    if (!action_queue) {
      _LOG_E(TAG, "Failed to create action queue");
    } else {
      BaseType_t ok = xTaskCreate(action_worker_task, "action_worker", 4096,
                                  NULL, 5, &action_task_handle);
      if (ok != pdPASS) {
        _LOG_E(TAG, "Failed to create action worker task");
        action_task_handle = NULL;
      }
    }
  }

  if (s_server) {
    _LOG_I(TAG, "Webserver already running");
    return s_server;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  esp_err_t ret = httpd_start(&s_server, &config);
  if (ret != ESP_OK) {
    _LOG_E(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
    s_server = NULL;
    return NULL;
  }

  static const httpd_uri_t uri_root = {.uri = "/",
                                       .method = HTTP_GET,
                                       .handler = root_get_handler,
                                       .user_ctx = NULL};
  static const httpd_uri_t uri_action_post = {.uri = "/action",
                                              .method = HTTP_POST,
                                              .handler = action_post_handler,
                                              .user_ctx = NULL};
  static const httpd_uri_t uri_action_options = {.uri = "/action",
                                                 .method = HTTP_OPTIONS,
                                                 .handler =
                                                     action_options_handler,
                                                 .user_ctx = NULL};
  static const httpd_uri_t uri_status = {.uri = "/status",
                                         .method = HTTP_GET,
                                         .handler = status_get_handler,
                                         .user_ctx = NULL};

  httpd_register_uri_handler(s_server, &uri_root);
  httpd_register_uri_handler(s_server, &uri_action_post);
  httpd_register_uri_handler(s_server, &uri_action_options);
  httpd_register_uri_handler(s_server, &uri_status);

  _LOG_I(TAG, "Webserver started");
  return s_server;
}

void stop_webserver(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    _LOG_I(TAG, "Webserver stopped");
  }
}
