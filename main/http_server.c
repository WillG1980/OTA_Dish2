#include <dishwasher_programs.h>
#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>



/* ---------- Handlers ---------- */

/* Root handler: serves HTML with buttons */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    // Build HTML page
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html><head><title>Dishwasher</title></head><body>");
    httpd_resp_sendstr_chunk(req, "<h2>Dishwasher Controls</h2>");

    for (int i = 0; i < ACTION_MAX; i++) {
        httpd_resp_sendstr_chunk(req, "<form action=\"/action\" method=\"get\">");
        httpd_resp_sendstr_chunk(req, "<input type=\"hidden\" name=\"action\" value=\"");
        httpd_resp_sendstr_chunk(req, action_names[i]);
        httpd_resp_sendstr_chunk(req, "\">");
        httpd_resp_sendstr_chunk(req, "<button type=\"submit\">");
        httpd_resp_sendstr_chunk(req, action_names[i]);
        httpd_resp_sendstr_chunk(req, "</button>");
        httpd_resp_sendstr_chunk(req, "</form><br>");
    }

    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL); // end response
    return ESP_OK;
}

/* Action handler: responds to button clicks */
static esp_err_t action_get_handler(httpd_req_t *req)
{
    char buf[64];
    int buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > sizeof(buf)) buf_len = sizeof(buf);

    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "action", param, sizeof(param)) == ESP_OK) {
            ESP_LOGI(TAG, "Action=%s", param);

            // Echo back chosen action
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, param);
            return ESP_OK;
        }
    }
    httpd_resp_send_400(req);
    return ESP_FAIL;
}

/* ---------- URI Table ---------- */

static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_action = {
    .uri       = "/action",
    .method    = HTTP_GET,
    .handler   = action_get_handler,
    .user_ctx  = NULL
};

/* ---------- Server Start/Stop ---------- */

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    ESP_LOGI(TAG, "Starting server on port %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_action);
        return server;
    }
    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}

void stop_webserver(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
    }
}
