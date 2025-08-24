// http_server.c — grouped actions, root UI, and queue-depth logging
// Adapted to your actions_t (CYCLE/DO/TOGGLE/ADMIN) and ACTIONS_TABLE mapping.
// - Root "/" serves an HTML UI with grouped buttons from ACTIONS_TABLE
// - /status supports GET and POST (JSON chunked)
// - Each action has POST endpoint: /action/<GROUP>/<NAME>
// - After any action enqueue, we log queue depth
// - Perform_action_<GROUP>_<NAME>() run in an action worker (not httpd)

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
#include "dishwasher_programs.h"   // extern status_struct ActiveStatus; run_program()
#include "local_ota.h"             // check_and_perform_ota()

#ifndef TAG
#define TAG "http_server"
#endif

#define ACTION_QUEUE_LEN   16
#define ACTION_TASK_STACK  4096
#define ACTION_TASK_PRIO   5
#define RUN_PROGRAM_STACK  8192

// ──────────────────────────────────────────────────────────────────────────────
// Utilities & globals
// ──────────────────────────────────────────────────────────────────────────────
static httpd_handle_t s_server       = NULL;
static QueueHandle_t  s_action_queue = NULL;
static TaskHandle_t   s_action_task  = NULL;
static TaskHandle_t   s_program_task = NULL; // single-run guard

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static inline unsigned queue_depth(void) { return s_action_queue ? (unsigned)uxQueueMessagesWaiting(s_action_queue) : 0u; }

static const char* ms_to_mmss(int64_t ms, char out[8]) {
    if (ms < 0) { strcpy(out, "--:--"); return out; }
    int64_t secs = ms / 1000; int mm = (int)(secs / 60); int ss = (int)(secs % 60);
    snprintf(out, 8, "%02d:%02d", mm, ss); return out;
}

static void drain_body(httpd_req_t *req) {
    int remaining = req->content_len; char tmp[128];
    while (remaining > 0) { int r = httpd_req_recv(req, tmp, (remaining > (int)sizeof(tmp)) ? sizeof(tmp) : remaining); if (r <= 0) break; remaining -= r; }
}

static bool has_token_ci(const char *s, const char *token) {
    if (!s || !token) return false; 
    size_t n = strlen(token);
    for (const char *p = s; *p; ++p) 
    if (strncasecmp(p, token, n) == 0) 
        return true; 
return false;
}

// ──────────────────────────────────────────────────────────────────────────────
// Status JSON (GET/POST)
// ──────────────────────────────────────────────────────────────────────────────
static esp_err_t handle_status(httpd_req_t *req) {
    drain_body(req);

    int64_t start_ms = (ActiveStatus.time_full_start > 0) ? ActiveStatus.time_full_start : ActiveStatus.time_start;
    int64_t total_ms = (ActiveStatus.time_full_total > 0) ? ActiveStatus.time_full_total : ActiveStatus.time_total;

    static bool soap_sticky = false; static int64_t last_prog_start = -1;
    if (start_ms > 0 && start_ms != last_prog_start) { last_prog_start = start_ms; soap_sticky = false; }

    int64_t elapsed_ms = -1;
    if (ActiveStatus.time_elapsed >= 0) elapsed_ms = ActiveStatus.time_elapsed; else if (start_ms > 0) {
        elapsed_ms = now_ms() - start_ms; if (elapsed_ms < 0) elapsed_ms = 0;
    }

    int64_t remaining_ms = -1;
    if (ActiveStatus.time_total > 0 && ActiveStatus.time_elapsed >= 0) remaining_ms = ActiveStatus.time_total - ActiveStatus.time_elapsed;
    else if (start_ms > 0 && total_ms > 0) remaining_ms = (start_ms + total_ms) - now_ms();
    if (remaining_ms < 0) remaining_ms = 0;

    if (has_token_ci(ActiveStatus.ActiveDevices, "soap") || has_token_ci(ActiveStatus.Step, "soap")) soap_sticky = true;

    httpd_resp_set_type(req, "application/json"); char buf[128], mm1[8], mm2[8], mm3[8];
    httpd_resp_sendstr_chunk(req, "{");
    httpd_resp_sendstr_chunk(req, "\"Program\":\""); httpd_resp_sendstr_chunk(req, ActiveStatus.Program); httpd_resp_sendstr_chunk(req, "\",");
    httpd_resp_sendstr_chunk(req, "\"name_cycle\":\""); httpd_resp_sendstr_chunk(req, ActiveStatus.Cycle); httpd_resp_sendstr_chunk(req, "\",");
    httpd_resp_sendstr_chunk(req, "\"name_step\":\""); httpd_resp_sendstr_chunk(req, ActiveStatus.Step); httpd_resp_sendstr_chunk(req, "\",");
    snprintf(buf, sizeof(buf), "\"CurrentTemp\":%d,", ActiveStatus.CurrentTemp); httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, "\"since_start_mmss\":\""); httpd_resp_sendstr_chunk(req, ms_to_mmss(elapsed_ms, mm1)); httpd_resp_sendstr_chunk(req, "\",");
    httpd_resp_sendstr_chunk(req, "\"remaining_mmss\":\""); httpd_resp_sendstr_chunk(req, ms_to_mmss(remaining_ms, mm2)); httpd_resp_sendstr_chunk(req, "\",");
    httpd_resp_sendstr_chunk(req, "\"eta_finish_mmss\":\""); httpd_resp_sendstr_chunk(req, ms_to_mmss(remaining_ms, mm3)); httpd_resp_sendstr_chunk(req, "\",");
    httpd_resp_sendstr_chunk(req, "\"soap_has_dispensed\":"); httpd_resp_sendstr_chunk(req, soap_sticky ? "true" : "false");
    httpd_resp_sendstr_chunk(req, "}\n"); httpd_resp_send_chunk(req, NULL, 0); return ESP_OK;
}

// ──────────────────────────────────────────────────────────────────────────────
// Perform_action_<GROUP>_<NAME>() — executed in action worker (not httpd)
// ──────────────────────────────────────────────────────────────────────────────
static bool start_program_if_idle(const char *program_name) {
    if (s_program_task && eTaskGetState(s_program_task) != eDeleted) { _LOG_W("run_program already active; ignoring duplicate START"); return false; }
    if (program_name && program_name[0]) { strncpy(ActiveStatus.Program, program_name, sizeof(ActiveStatus.Program) - 1); ActiveStatus.Program[sizeof(ActiveStatus.Program) - 1] = '\0'; }
    if (xTaskCreate(run_program, "run_program", RUN_PROGRAM_STACK, NULL, ACTION_TASK_PRIO, &s_program_task) != pdPASS) { _LOG_E("failed to create run_program task"); s_program_task = NULL; return false; }
    return true;
}

// CYCLE
void Perform_action_CYCLE_NORMAL(void)  { (void)start_program_if_idle("Normal"); }
void Perform_action_CYCLE_TESTER(void)  { (void)start_program_if_idle("Tester"); }
void Perform_action_CYCLE_HITEMP(void)  { (void)start_program_if_idle("HiTemp"); }
// DO
void Perform_action_DO_PAUSE(void)      { _LOG_I("Pause requested"); }
void Perform_action_DORESUME(void)      { _LOG_I("Resume requested"); }
// TOGGLE — wire these to your IO layer
void Perform_action_TOGGLE_DRAIN(void)  { _LOG_I("Toggle Drain"); }
void Perform_action_TOGGLE_FILL(void)   { _LOG_I("Toggle Fill"); }
void Perform_action_TOGGLE_SPRAY(void)  { _LOG_I("Toggle Spray"); }
void Perform_action_TOGGLE_HEAT(void)   { _LOG_I("Toggle Heat"); }
void Perform_action_TOGGLE_SOAP(void)   { _LOG_I("Toggle Soap"); }
void Perform_action_TOGGLE_LEDS(void)   { _LOG_I("Toggle LEDs"); }
// ADMIN
void Perform_action_ADMIN_CANCEL(void)        { _LOG_I("Cancel requested (implement cooperative stop)"); }
void Perform_action_ADMIN_FIRMWAREUPDATE(void){ _LOG_I("Performing UPDATE (OTA)"); check_and_perform_ota(); }
void Perform_action_ADMIN_REBOOT(void)        { _LOG_I("Performing REBOOT"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
void Perform_action_ADMIN_SKIP_STEP(void)     { _LOG_I("Skip step requested"); }

// ──────────────────────────────────────────────────────────────────────────────
// Action dispatch & worker
// ──────────────────────────────────────────────────────────────────────────────
#define DECLARE_PERFORM(GROUP, NAME, TOKEN) void Perform_action_##GROUP##_##NAME(void);
ACTIONS_TABLE(DECLARE_PERFORM)
#undef DECLARE_PERFORM

static void dispatch_action(actions_t a) {
#define DISPATCH_CASE(GROUP, NAME, TOKEN) case TOKEN: Perform_action_##GROUP##_##NAME(); break;
    switch (a) { ACTIONS_TABLE(DISPATCH_CASE) default: _LOG_W("Unknown action: %d", (int)a); break; }
#undef DISPATCH_CASE
}

static void action_worker(void *arg) {
    (void)arg; for (;;) { actions_t a; if (xQueueReceive(s_action_queue, &a, portMAX_DELAY) == pdTRUE) { dispatch_action(a); } }
}

// ──────────────────────────────────────────────────────────────────────────────
// HTTP action handlers (enqueue + queue depth logging)
// ──────────────────────────────────────────────────────────────────────────────
static esp_err_t handle_action_common(httpd_req_t *req, actions_t a) {
    drain_body(req);
    if (!s_action_queue) { httpd_resp_set_status(req, "503 Service Unavailable"); httpd_resp_sendstr(req, "queue not ready\n"); return ESP_OK; }
    if (xQueueSend(s_action_queue, &a, 0) != pdTRUE) { _LOG_W("action queue full; dropping %d", (int)a); httpd_resp_set_status(req, "503 Service Unavailable"); httpd_resp_sendstr(req, "queue full\n"); return ESP_OK; }
    unsigned depth = queue_depth(); _LOG_D("action enqueued: %d (queue depth now %u)", (int)a, depth);
    httpd_resp_set_type(req, "text/plain"); httpd_resp_sendstr(req, "OK\n"); return ESP_OK;
}

typedef struct { const char *uri; actions_t act; const char *group; const char *name; } route_t;
#define URI_ROW(GROUP, NAME, TOKEN) { "/action/" #GROUP "/" #NAME , TOKEN, #GROUP, #NAME },
static const route_t ROUTES[] = { ACTIONS_TABLE(URI_ROW) };
#undef URI_ROW

static esp_err_t generic_action_handler(httpd_req_t *req) {
    for (size_t i = 0; i < sizeof(ROUTES)/sizeof(ROUTES[0]); ++i) if (strcmp(req->uri, ROUTES[i].uri) == 0) return handle_action_common(req, ROUTES[i].act);
    httpd_resp_set_status(req, "404 Not Found"); httpd_resp_sendstr(req, "unknown action\n"); return ESP_OK;
}

static void register_action_routes(httpd_handle_t s) {
    for (size_t i = 0; i < sizeof(ROUTES)/sizeof(ROUTES[0]); ++i) { httpd_uri_t u = { .uri = ROUTES[i].uri, .method = HTTP_POST, .handler = generic_action_handler, .user_ctx = NULL }; httpd_register_uri_handler(s, &u); }
}

// ──────────────────────────────────────────────────────────────────────────────
// Root UI (GET "/") — 95% width status viewport + grouped buttons
// ──────────────────────────────────────────────────────────────────────────────
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset=\\"utf-8\\"><meta name=\\"viewport\\" content=\\"width=device-width, initial-scale=1\\">"
        "<title>Dishwasher</title>"
        "<style>body{font-family:sans-serif;margin:1rem} .row{margin:0.75rem 0}"
        ".btn{padding:0.6rem 1rem;margin:0.25rem;border:1px solid #ccc;border-radius:10px;cursor:pointer}"
        ".btn.pushed{background:#ddd} #status{width:95%;height:16rem;border:1px solid #ccc;padding:0.5rem;white-space:pre;overflow:auto}"
        ".group{font-weight:600;margin-right:0.5rem}</style></head><body>");

    const char *current_group = NULL;
    for (size_t i = 0; i < sizeof(ROUTES)/sizeof(ROUTES[0]); ++i) {
        const route_t *r = &ROUTES[i];
        if (!current_group || strcmp(current_group, r->group) != 0) {
            if (current_group) httpd_resp_sendstr_chunk(req, "</div>");
            httpd_resp_sendstr_chunk(req, "<div class=\"row\"><span class=\"group\">"); httpd_resp_sendstr_chunk(req, r->group); httpd_resp_sendstr_chunk(req, ":</span>");
            current_group = r->group;
        }
        httpd_resp_sendstr_chunk(req, "<button class=\"btn\" data-uri=\""); httpd_resp_sendstr_chunk(req, r->uri); httpd_resp_sendstr_chunk(req, "\">"); httpd_resp_sendstr_chunk(req, r->name); httpd_resp_sendstr_chunk(req, "</button>");
    }
    if (current_group) httpd_resp_sendstr_chunk(req, "</div>");

    httpd_resp_sendstr_chunk(req,
        "<h3>Status</h3><pre id=\"status\"></pre>"
        "<script>\n"
        "const statusBox=document.getElementById('status');\n"
        "async function refresh(){try{const r=await fetch('/status');const t=await r.text();statusBox.textContent=t;}catch(e){statusBox.textContent='(error fetching /status)'}}\n"
        "function pushMark(btn){btn.classList.add('pushed');setTimeout(()=>btn.classList.remove('pushed'),2000)}\n"
        "async function fire(uri,btn){pushMark(btn);try{await fetch(uri,{method:'POST'});}catch(e){} setTimeout(refresh,1000);}\n"
        "document.querySelectorAll('.btn').forEach(b=>b.addEventListener('click',()=>fire(b.dataset.uri,b)));\n"
        "setInterval(refresh,10000);\n"
        "refresh();\n"
        "</script>");

    httpd_resp_sendstr_chunk(req, "</body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// ──────────────────────────────────────────────────────────────────────────────
// Server lifecycle
// ──────────────────────────────────────────────────────────────────────────────
void start_webserver(void) {
    static bool started = false; if (started && s_server) { _LOG_I("webserver already started"); return; }

    if (!s_action_queue) { s_action_queue = xQueueCreate(ACTION_QUEUE_LEN, sizeof(actions_t)); if (!s_action_queue) { _LOG_E("failed to create action queue"); return; } }
    if (!s_action_task)   { if (xTaskCreate(action_worker, "action_worker", ACTION_TASK_STACK, NULL, ACTION_TASK_PRIO, &s_action_task) != pdPASS) { _LOG_E("failed to create action_worker"); return; } }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &config) != ESP_OK) { _LOG_E("httpd_start failed"); return; }

    httpd_uri_t status_get  = { .uri="/status", .method=HTTP_GET , .handler=handle_status, .user_ctx=NULL };
    httpd_uri_t status_post = { .uri="/status", .method=HTTP_POST, .handler=handle_status, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &status_get); httpd_register_uri_handler(s_server, &status_post);

    register_action_routes(s_server);

    httpd_uri_t root_get = { .uri="/", .method=HTTP_GET, .handler=root_get_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &root_get);

    started = true; _LOG_I("webserver started");
}

void stop_webserver(void) { if (s_server) httpd_stop(s_server); s_server = NULL; _LOG_I("webserver stopped"); }

bool http_server_is_running(void) { return s_server != NULL; }
