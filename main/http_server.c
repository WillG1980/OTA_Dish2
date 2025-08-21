#include <dishwasher_programs.h>
#include <string.h>
#include <strings.h>           // strcasecmp
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_server.h"


typedef enum {
    ACTION_START,
    ACTION_CANCEL,
    ACTION_STATUS,
    ACTION_TEST,
    ACTION_MAX
} actions_t;

static const char *action_names[ACTION_MAX] = { "Start", "Cancel", "Status", "Test" };

/* ---- background worker ---- */
static QueueHandle_t action_queue = NULL;
static TaskHandle_t action_task_handle = NULL;

static void perform_action(actions_t action)
{
    switch (action) {
        case ACTION_START:  _LOG_I(TAG, "Performing START");  break;
        case ACTION_CANCEL: _LOG_I(TAG, "Performing CANCEL"); break;
        case ACTION_STATUS: _LOG_I(TAG, "Performing STATUS"); break;
        case ACTION_TEST:   _LOG_I(TAG, "Performing TEST");   break;
        default:            _LOG_W(TAG, "Unknown action %d", action); break;
    }
}

static void action_task(void *arg)
{
    actions_t act;
    for (;;) {
        if (xQueueReceive(action_queue, &act, portMAX_DELAY)) {
            perform_action(act);
        }
    }
}

/* Call this once from app_main() before starting the webserver */
void http_server_actions_init(void)
{
    if (!action_queue) {
        action_queue = xQueueCreate(8, sizeof(actions_t));
        configASSERT(action_queue);
    }
    if (!action_task_handle) {
        BaseType_t ok = xTaskCreate(action_task, "action_task", 4096, NULL, 5, &action_task_handle);
        configASSERT(ok == pdPASS);
    }
}

/* ---- helpers ---- */
static actions_t parse_action(const char *param)
{
    for (int i = 0; i < ACTION_MAX; i++) {
        if (strcasecmp(param, action_names[i]) == 0) return (actions_t)i;
    }
    return ACTION_MAX;
}

/* ---- HTTP handlers ---- */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Dishwasher</title>"
        "<style>body{font-family:sans-serif;margin:24px}"
        ".btn{display:inline-block;margin:6px 8px;padding:10px 16px;border:0;border-radius:10px;"
        "box-shadow:0 1px 3px rgba(0,0,0,.15);cursor:pointer}</style></head><body><h2>Dishwasher Controls</h2><div>");
    for (int i = 0; i < ACTION_MAX; i++) {
        httpd_resp_sendstr_chunk(req, "<button class='btn' onclick='fetch(\"/action?action=");
        httpd_resp_sendstr_chunk(req, action_names[i]);
        httpd_resp_sendstr_chunk(req, "\").then(r=>r.text()).then(t=>alert(t))'>");
        httpd_resp_sendstr_chunk(req, action_names[i]);
        httpd_resp_sendstr_chunk(req, "</button>");
    }
    httpd_resp_sendstr_chunk(req, "</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t action_get_handler(httpd_req_t *req)
{
    char buf[64];
    int buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > (int)sizeof(buf)) buf_len = sizeof(buf);

    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "action", param, sizeof(param)) == ESP_OK) {
            actions_t act = parse_action(param);

            httpd_resp_set_type(req, "text/plain");
            if (act == ACTION_MAX) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
                return ESP_FAIL;
            }
            /* respond immediately */
            httpd_resp_sendstr(req, param);

            /* hand off to background task (non-blocking) */
            if (xQueueSend(action_queue, &act, 0) != pdPASS) {
                ESP_LOGW(TAG, "Action queue full; dropping %s", param);
            }
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
    return ESP_FAIL;
}

/* ---- URI table & server start/stop ---- */
static const httpd_uri_t uri_root = { .uri="/", .method=HTTP_GET, .handler=root_get_handler, .user_ctx=NULL };
static const httpd_uri_t uri_action = { .uri="/action", .method=HTTP_GET, .handler=action_get_handler, .user_ctx=NULL };

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_action);
    } else {
        _LOG_E(TAG, "httpd_start failed");
    }
    return server;
}

void stop_webserver(httpd_handle_t server)
{
    if (server) httpd_stop(server);
    /* optional: stop worker */
    // if (action_task_handle) { vTaskDelete(action_task_handle); action_task_handle = NULL; }
    // if (action_queue) { vQueueDelete(action_queue); action_queue = NULL; }
}
