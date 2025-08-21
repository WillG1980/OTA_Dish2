#include <dishwasher_programs.h>
#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>


/* Your enum of actions */
typedef enum {
    ACTION_START,
    ACTION_CANCEL,
    ACTION_STATUS,
    ACTION_TEST,
    ACTION_MAX
} actions_t;

/* Matching names for actions */
static const char *action_names[ACTION_MAX] = {
    "Start",
    "Cancel",
    "Status",
    "Test"
};

/* ---------- Handlers ---------- */

/* Root handler: serves HTML with buttons */
static esp_err_t root_get_handler(httpd_req_t *req);
/* Action handler: responds to button clicks */
static esp_err_t action_get_handler(httpd_req_t *req);

/* ---------- Server Start/Stop ---------- */

httpd_handle_t start_webserver(void);

void stop_webserver(httpd_handle_t server);