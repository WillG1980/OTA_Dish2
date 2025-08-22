// http_server.c — replacement
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>  // strcasecmp

// Project headers (adjust if your paths differ)
#include "http_server.h"
#include "local_ota.h"            // check_and_perform_ota()
#include "dishwasher_programs.h"  // run_program(), ActiveStatus, setCharArray(...)

#define TAG "http_server"

// --- Actions enum (match your existing enum if it already exists) ---
typedef enum {
  ACTION_NONE = 0,
  ACTION_START,
  ACTION_TEST,
  ACTION_HITEMP,
  ACTION_CANCEL,
  ACTION_UPDATE,
  ACTION_REBOOT
} actions_t;

// --- Queue / Tasks ---
static QueueHandle_t action_queue = NULL;
static TaskHandle_t action_task_handle = NULL;

// Only-one-run guard:
static TaskHandle_t program_task_handle = NULL;

// HTTPD handle and "once" guards
static httpd_handle_t s_server = NULL;
static bool s_initialized = false;

// ------------------ Utilities ------------------

static actions_t action_from_str(const char *s) {
  if (!s) return ACTION_NONE;
  if (!strcasecmp(s, "Start"))  return ACTION_START;
  if (!strcasecmp(s, "Test"))   return ACTION_TEST;
  if (!strcasecmp(s, "HiTemp")) return ACTION_HITEMP;
  if (!strcasecmp(s, "Cancel")) return ACTION_CANCEL;
  if (!strcasecmp(s, "Update")) return ACTION_UPDATE;
  if (!strcasecmp(s, "Reboot")) return ACTION_REBOOT;
  return ACTION_NONE;
}

static void program_task_trampoline(void *arg) {
  // run_program() is expected to be a FreeRTOS task entry (void(*)(void*))
  run_program(arg);
  // clear the singleton handle when finished
  program_task_handle = NULL;
  vTaskDelete(NULL);
}

// Start a program only if none is running
static void start_program_if_idle(const char *name) {
  if (program_task_handle != NULL) {
    ESP_LOGW(TAG, "Program already running; ignoring Start/Test/HiTemp request");
    return;
  }

  // Set visible status then start
  setCharArray(ActiveStatus.Program, name);
  BaseType_t ok = xTaskCreate(
      program_task_trampoline, "run_program", 8192, NULL, 5, &program_task_handle);
  if (ok != pdPASS) {
    program_task_handle = NULL;
    ESP_LOGE(TAG, "Failed to create run_program task");
  } else {
    ESP_LOGI(TAG, "Started program '%s'", name);
  }
}

// ------------------ Action execution (consumer side) ------------------

static void perform_action(actions_t action) {
  switch (action) {
    case ACTION_START:
      start_program_if_idle("Normal");
      break;
    case ACTION_TEST:
      start_program_if_idle("Tester");
      break;
    case ACTION_HITEMP:
      start_program_if_idle("HiTemp");
      break;
    case ACTION_CANCEL:
      ESP_LOGI(TAG, "Cancel requested");
      // If you have a cooperative cancel mechanism, trigger it here.
      // Example:
      // setCharArray(ActiveStatus.Program, "Idle");
      // notify_run_program_cancel();
      break;
    case ACTION_UPDATE:
      ESP_LOGI(TAG, "Update requested");
      check_and_perform_ota();  // Non-blocking preferred if available
      break;
    case ACTION_REBOOT:
      ESP_LOGI(TAG, "Reboot requested");
      esp_restart();
      break;
    default:
      ESP_LOGW(TAG, "Unknown or no-op action");
  }
}

static void action_worker_task(void *arg) {
  actions_t act;
  for (;;) {
    if (xQueueReceive(action_queue, &act, portMAX_DELAY) == pdPASS) {
      // Log queue depth on each processed item (messages waiting AFTER dequeue)
      UBaseType_t pending = uxQueueMessagesWaiting(action_queue);
      ESP_LOGI(TAG, "Action dequeued=%d, queue depth now=%u", (int)act, (unsigned)pending);
      perform_action(act);
    }
  }
}

// ------------------ HTTP helpers ------------------

static void set_common_headers(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  // If you want cross-origin access for a web UI hosted elsewhere, leave '*' or set a specific origin.
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

// Parse ?action= from URL query
static actions_t parse_action_from_query(httpd_req_t *req) {
  char buf[64];
  actions_t a = ACTION_NONE;

  size_t qs_len = httpd_req_get_url_query_len(req) + 1;
  if (qs_len > 1 && qs_len < sizeof(buf)) {
    if (httpd_req_get_url_query_str(req, buf, qs_len) == ESP_OK) {
      char val[32];
      if (httpd_query_key_value(buf, "action", val, sizeof(val)) == ESP_OK) {
        a = action_from_str(val);
      }
    }
  }
  return a;
}

// Parse small POST body as either "action=..." (form) or {"action":"..."} (simple JSON)
static actions_t parse_action_from_body(httpd_req_t *req) {
  actions_t a = ACTION_NONE;

  int total = req->content_len;
  if (total <= 0 || total > 256) return ACTION_NONE;  // keep it tiny/safe

  char buf[257];
  int recvd = httpd_req_recv(req, buf, total);
  if (recvd <= 0) return ACTION_NONE;
  buf[recvd] = '\0';

  // Try form-encoded first: action=Start
  const char *p = strstr(buf, "action=");
  if (p) {
    p += 7;
    // read until '&' or end
    char val[32] = {0};
    int i = 0;
    while (*p && *p != '&' && i < (int)sizeof(val) - 1) {
      val[i++] = *p++;
    }
    a = action_from_str(val);
    if (a != ACTION_NONE) return a;
  }

  // Try minimal JSON: {"action":"Start"}
  const char *q = strstr(buf, "\"action\"");
  if (q) {
    q = strchr(q, ':');
    if (q) {
      q++;
      while (*q == ' ' || *q == '\t') q++;
      if (*q == '"' || *q == '\'') {
        char quote = *q++;
        char val[32] = {0};
        int i = 0;
        while (*q && *q != quote && i < (int)sizeof(val) - 1) {
          val[i++] = *q++;
        }
        a = action_from_str(val);
      }
    }
  }
  return a;
}

// ------------------ HTTP Handlers ------------------

// Root: simple page with POST buttons
static const char *INDEX_HTML =
"<!doctype html><meta name=viewport content='width=device-width, initial-scale=1'>"
"<title>Dishwasher</title>"
"<style>button{margin:4px;padding:10px 16px;font-size:16px}</style>"
"<h1>Dishwasher Controls</h1>"
"<div>"
"<button onclick='doPost(\"Start\")'>Start</button>"
"<button onclick='doPost(\"Test\")'>Test</button>"
"<button onclick='doPost(\"HiTemp\")'>HiTemp</button>"
"<button onclick='doPost(\"Cancel\")'>Cancel</button>"
"<button onclick='doPost(\"Update\")'>Update</button>"
"<button onclick='doPost(\"Reboot\")'>Reboot</button>"
"</div>"
"<pre id=out></pre>"
"<script>"
"async function doPost(action){"
"  const r = await fetch(\"/action\", {method:\"POST\", headers:{\"Content-Type\":\"application/x-www-form-urlencoded\"}, body:\"action=\"+encodeURIComponent(action), cache:\"no-store\", credentials:\"same-origin\"});"
"  const t = await r.text();"
"  document.getElementById('out').textContent = (r.ok?\"OK \":\"ERR \")+t;"
"}"
"</script>";

static esp_err_t root_get_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_sendstr(req, INDEX_HTML);
}

// Unified /action POST handler
static esp_err_t action_post_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");

  actions_t act = ACTION_NONE;

  // Prefer body for POST, but also accept ?action= in URL
  act = parse_action_from_body(req);
  if (act == ACTION_NONE) {
    act = parse_action_from_query(req);
  }
  if (act == ACTION_NONE) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "missing or invalid action");
    return ESP_OK;
  }

  if (!action_queue) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "action queue not ready");
    return ESP_OK;
  }

  if (xQueueSend(action_queue, &act, 0) != pdPASS) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "busy");
    return ESP_OK;
  }

  // Report immediate queue depth after enqueue (optional)
  UBaseType_t pending = uxQueueMessagesWaiting(action_queue);
  char msg[64];
  snprintf(msg, sizeof(msg), "queued; queue depth=%u\n", (unsigned)pending);
  httpd_resp_sendstr(req, msg);
  return ESP_OK;
}

// Optional: still support GET /action?action=Start (compat)
static esp_err_t action_get_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  actions_t act = parse_action_from_query(req);
  if (act == ACTION_NONE) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "missing or invalid action");
    return ESP_OK;
  }
  if (!action_queue || xQueueSend(action_queue, &act, 0) != pdPASS) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_sendstr(req, "busy");
    return ESP_OK;
  }
  UBaseType_t pending = uxQueueMessagesWaiting(action_queue);
  char msg[64];
  snprintf(msg, sizeof(msg), "queued; queue depth=%u\n", (unsigned)pending);
  httpd_resp_sendstr(req, msg);
  return ESP_OK;
}

// CORS preflight (for cross-origin UI)
static esp_err_t action_options_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); // narrow if possible
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_sendstr(req, "");
  return ESP_OK;
}

// Example /status page (kept minimal; adjust to your real status output)
static esp_err_t status_get_handler(httpd_req_t *req) {
  set_common_headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr_chunk(req, "<h1>Status</h1><pre>");
  httpd_resp_sendstr_chunk(req, "Program: ");
  httpd_resp_sendstr_chunk(req, ActiveStatus.Program);
  httpd_resp_sendstr_chunk(req, "\n");
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

// ------------------ Server start (with guards) ------------------

httpd_handle_t start_webserver(void) {
  // One-time init guarded here (safe if called multiple times)
  if (!s_initialized) {
    s_initialized = true;

    action_queue = xQueueCreate(16, sizeof(actions_t));
    if (!action_queue) {
      ESP_LOGE(TAG, "Failed to create action queue");
    } else {
      BaseType_t ok = xTaskCreate(action_worker_task, "action_worker", 4096, NULL, 5, &action_task_handle);
      if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create action worker task");
        action_task_handle = NULL;
      }
    }
  }

  if (s_server) {
    ESP_LOGI(TAG, "Webserver already running");
    return s_server;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // You may want to adjust stack size, core affinity, etc., here.

  esp_err_t ret = httpd_start(&s_server, &config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
    s_server = NULL;
    return NULL;
  }

  static const httpd_uri_t uri_root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = root_get_handler,
      .user_ctx = NULL};

  static const httpd_uri_t uri_action_post = {
      .uri = "/action",
      .method = HTTP_POST,
      .handler = action_post_handler,
      .user_ctx = NULL};

  static const httpd_uri_t uri_action_get = {
      .uri = "/action",
      .method = HTTP_GET,
      .handler = action_get_handler,   // keep for compatibility; remove if you want POST-only
      .user_ctx = NULL};

  static const httpd_uri_t uri_action_options = {
      .uri = "/action",
      .method = HTTP_OPTIONS,
      .handler = action_options_handler,
      .user_ctx = NULL};

  static const httpd_uri_t uri_status = {
      .uri = "/status",
      .method = HTTP_GET,
      .handler = status_get_handler,
      .user_ctx = NULL};

  httpd_register_uri_handler(s_server, &uri_root);
  httpd_register_uri_handler(s_server, &uri_action_post);
  httpd_register_uri_handler(s_server, &uri_action_get);
  httpd_register_uri_handler(s_server, &uri_action_options);
  httpd_register_uri_handler(s_server, &uri_status);

  ESP_LOGI(TAG, "Webserver started");
  return s_server;
}

void stop_webserver(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Webserver stopped");
  }
}
