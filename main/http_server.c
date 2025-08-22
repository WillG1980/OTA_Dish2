// main/http_server.c
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <dishwasher_programs.h>   // run_program, ActiveStatus, setCharArray, etc.
#include <string.h>
#include <strings.h>               // strcasecmp
#include <inttypes.h>

#include "local_ota.h"             // check_and_perform_ota()
#include "http_server.h"           // actions_t, action_names[], ACTION_*

#ifndef TAG
#define TAG "http_server"
#endif

/* ====================== background worker ====================== */
static QueueHandle_t action_queue = NULL;
static TaskHandle_t  action_task_handle = NULL;

/* ====================== time/format helpers ==================== */
// If total is an END timestamp (>= start), use it; else treat as DURATION added to start.
static inline int64_t remaining_us_from(int64_t start, int64_t total_or_end, int64_t now_us) {
    int64_t end_us = (total_or_end >= start) ? total_or_end : (start + total_or_end);
    int64_t rem = end_us - now_us;
    return rem > 0 ? rem : 0;
}
static inline void fmt_hms(char *dst, size_t cap, int64_t us) {
    int64_t s = us / 1000000;
    long long h = (long long)(s / 3600);
    long long m = (long long)((s % 3600) / 60);
    long long sec = (long long)(s % 60);
    snprintf(dst, cap, "%lld:%02lld:%02lld", h, m, sec);
}
/* ====================== action execution ======================= */
static void perform_action(actions_t action) {
    switch (action) {
    case ACTION_START:
        _LOG_I("Performing Normal");
        setCharArray(ActiveStatus.Program, "Normal"); \
        xTaskCreate(run_program, "run_program", 8192, NULL, 5, NULL);
        break;
    case ACTION_TEST:
        _LOG_I("Performing TEST");
        setCharArray(ActiveStatus.Program, "Tester");
        xTaskCreate(run_program, "run_program", 8192, NULL, 5, NULL);
        break;
    case ACTION_HiTemp:
        _LOG_I("Performing HiTemp Wash");
        setCharArray(ActiveStatus.Program, "HiTemp");
        xTaskCreate(run_program, "run_program", 8192, NULL, 5, NULL);
        break;

    case ACTION_CANCEL:
        gpio_mask_clear(HEAT | SPRAY | INLET | DRAIN | SOAP); // set all pins to off
        _LOG_I("Performing CANCEL");
        setCharArray(ActiveStatus.Step,    "Cancel");
        setCharArray(ActiveStatus.Cycle,   "Cancel");
        setCharArray(ActiveStatus.Program, "Cancel");
        ActiveStatus.HEAT_REQUESTED = false;
        esp_restart();
        break;

    case ACTION_STATUS:
        _LOG_I("Performing STATUS (no-op; panel polls /status)");
        break;


    case ACTION_UPDATE:
        _LOG_I("Performing UPDATE");
        check_and_perform_ota();
        break;

    case ACTION_REBOOT:
        _LOG_I("Performing REBOOT");
        esp_restart();
        break;

    default:
        _LOG_W("Unknown action %d", action);
        break;
    }
}

static void action_task(void *arg) {
    actions_t act;
    for (;;) {
        if (xQueueReceive(action_queue, &act, portMAX_DELAY)) {
            _LOG_I("Received action %s", action_names[act]);
            perform_action(act);
        }
    }
}

/* Call this once from app_main() before starting the webserver */
void http_server_actions_init(void) {
    if (!action_queue) {
        action_queue = xQueueCreate(8, sizeof(actions_t));
        configASSERT(action_queue);
    }
    if (!action_task_handle) {
        BaseType_t ok = xTaskCreate(action_task, "action_task", 4096, NULL, 5, &action_task_handle);
        configASSERT(ok == pdPASS);
    }
}

/* ====================== helpers ================================ */
static actions_t parse_action(const char *param) {
    for (int i = 0; i < ACTION_MAX; i++) {
        if (strcasecmp(param, action_names[i]) == 0) return (actions_t)i;
    }
    return ACTION_MAX;
}

/* ====================== HTTP handlers ========================== */
static esp_err_t status_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    int64_t now = esp_timer_get_time();

    // Snapshot values to avoid torn reads while tasks update status.
    char program[48], cycle[48], step[48];
    snprintf(program, sizeof(program), "%s", ActiveStatus.Program);
    snprintf(cycle,   sizeof(cycle),   "%s", ActiveStatus.Cycle);
    snprintf(step,    sizeof(step),    "%s", ActiveStatus.Step);

    int64_t rem_full  = remaining_us_from(ActiveStatus.time_full_start,
                                          ActiveStatus.time_full_total,  now);
    int64_t rem_cycle = remaining_us_from(ActiveStatus.time_cycle_start,
                                          ActiveStatus.time_cycle_total, now);

    char full_hms[32], cycle_hms[32];
    fmt_hms(full_hms,  sizeof full_hms,  rem_full);
    fmt_hms(cycle_hms, sizeof cycle_hms, rem_cycle);

#ifndef PRIx64
#define PRIx64 "llx"
#endif
    char devices_hex[24];
    snprintf(devices_hex, sizeof devices_hex, "0x%016" PRIx64, (uint64_t)ActiveStatus.ActiveDevices);

    httpd_resp_sendstr_chunk(req,
        "<div style='font-family:system-ui,Segoe UI,Roboto,Arial;font-size:14px'>"
        "<table style='width:100%;border-collapse:collapse'>"
        "<tr>"
          "<th style=\"text-align:left;padding:4px;border-bottom:1px solid #ddd\">Field</th>"
          "<th style=\"text-align:left;padding:4px;border-bottom:1px solid #ddd\">Value</th>"
        "</tr>");

    char row[256];
    snprintf(row, sizeof row, "<tr><td style='padding:4px'>Program</td><td style='padding:4px'>%s</td></tr>", program);
    httpd_resp_sendstr_chunk(req, row);
    snprintf(row, sizeof row, "<tr><td style='padding:4px'>Cycle</td><td style='padding:4px'>%s</td></tr>", cycle);
    httpd_resp_sendstr_chunk(req, row);
    snprintf(row, sizeof row, "<tr><td style='padding:4px'>Step</td><td style='padding:4px'>%s</td></tr>", step);
    httpd_resp_sendstr_chunk(req, row);
    snprintf(row, sizeof row, "<tr><td style='padding:4px'>Remaining (Program)</td><td style='padding:4px'>%s</td></tr>", full_hms);
    httpd_resp_sendstr_chunk(req, row);
    snprintf(row, sizeof row, "<tr><td style='padding:4px'>Remaining (Cycle)</td><td style='padding:4px'>%s</td></tr>", cycle_hms);
    httpd_resp_sendstr_chunk(req, row);
    snprintf(row, sizeof row, "<tr><td style='padding:4px'>Active Devices</td><td style='padding:4px'>%s</td></tr>", devices_hex);
    httpd_resp_sendstr_chunk(req, row);

    httpd_resp_sendstr_chunk(req, "</table></div>");
    return httpd_resp_sendstr_chunk(req, NULL); // end chunked response
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    const esp_app_desc_t *app = esp_app_get_description();
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");

    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Dishwasher</title>"
        "<style>"
          "body{font-family:sans-serif;margin:24px}"
          ".btn{display:inline-block;margin:6px 8px;padding:10px 16px;"
               "border:0;border-radius:10px;"
               "box-shadow:0 1px 3px rgba(0,0,0,.15);cursor:pointer}"
          "#status-panel{width:95%;margin:16px auto;padding:12px;border:1px solid #ddd;"
               "border-radius:10px;box-shadow:0 1px 3px rgba(0,0,0,.08);min-height:64px}"
        "</style></head><body><h2>Dishwasher Controls (");

    httpd_resp_sendstr_chunk(req, app->project_name); // e.g. "OTA_Dish2"
    httpd_resp_sendstr_chunk(req, " - ");
    httpd_resp_sendstr_chunk(req, app->version);      // e.g. "126" or "1.2.3"
    httpd_resp_sendstr_chunk(req, ")</h2><div>");

    // Buttons for all actions
    for (int i = 0; i < ACTION_MAX; i++) {
        httpd_resp_sendstr_chunk(req,
            "<button class='btn' onclick='fetch(\"/action?action=");
        httpd_resp_sendstr_chunk(req, action_names[i]);
        httpd_resp_sendstr_chunk(req,
            "\").then(r=>r.text()).then(t=>console.log(t))'>");
        httpd_resp_sendstr_chunk(req, action_names[i]);
        httpd_resp_sendstr_chunk(req, "</button>");
    }
    httpd_resp_sendstr_chunk(req, "</div>");

    // Status panel (auto-refreshes)
    httpd_resp_sendstr_chunk(req,
        "<div id='status-panel'><em>Loading status…</em></div>"
        "<script>"
          "async function fetchStatus(){"
            "try{"
              "const r=await fetch('/status',{cache:'no-store'});"
              "const t=await r.text();"
              "document.getElementById('status-panel').innerHTML=t;"
            "}catch(e){"
              "document.getElementById('status-panel').innerHTML="
              "'<span style=\"color:#b00\">Status fetch failed</span>';"
            "}"
          "}"
          "window.addEventListener('load',()=>{fetchStatus();setInterval(fetchStatus,2000);});"
        "</script></body></html>");

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t action_get_handler(httpd_req_t *req) {
    char buf[64];

    int qlen = httpd_req_get_url_query_len(req);
    if (qlen <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }
    int buf_len = qlen + 1;
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
                _LOG_W(TAG, "Action queue full; dropping %s", param);
            }
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
    return ESP_FAIL;
}

/* ====================== URI table & server start/stop ========== */
static const httpd_uri_t uri_root = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = root_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_action = {
    .uri      = "/action",
    .method   = HTTP_GET,
    .handler  = action_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_status = {
    .uri      = "/status",
    .method   = HTTP_GET,
    .handler  = status_get_handler,
    .user_ctx = NULL
};

httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_action);
        httpd_register_uri_handler(server, &uri_status);
    } else {
        _LOG_E(TAG, "httpd_start failed");
    }
    return server;
}

void stop_webserver(httpd_handle_t server) {
    if (server) httpd_stop(server);
    /* optional: stop worker
    if (action_task_handle) { vTaskDelete(action_task_handle); action_task_handle = NULL; }
    if (action_queue)       { vQueueDelete(action_queue);      action_queue = NULL; }
    */
}
