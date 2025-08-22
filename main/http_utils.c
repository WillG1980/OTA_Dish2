#include "dishwasher_programs.h"
#include "http_utils.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <string.h>


#define HTTP_WRAPPER_BUF_DEFAULT 16384  // 16 KiB

#ifndef HTTP_WRAPPER_TIMEOUT_MS
#define HTTP_WRAPPER_TIMEOUT_MS 5000
#endif

static int s_last_status = 0;

int http_get_last_status(void) {
    return s_last_status;
}

// Legacy API preserved: caller must free() the returned buffer.
char* _http_get(const char *url, int *out_len, int *out_status)
{
    if (!url) {
        if (out_len)    *out_len = 0;
        if (out_status) *out_status = 0;
        return NULL;
    }

    char *buf = (char*)malloc(HTTP_WRAPPER_BUF_DEFAULT);
    if (!buf) {
        if (out_len)    *out_len = 0;
        if (out_status) *out_status = 0;
        return NULL;
    }

    esp_err_t err = http_get(url, buf, HTTP_WRAPPER_BUF_DEFAULT, HTTP_WRAPPER_TIMEOUT_MS);

    // Fill outs (status first so it's set even on failure)
    if (out_status) *out_status = http_get_last_status();

    if (err != ESP_OK) {
        if (out_len) *out_len = 0;
        free(buf);
        return NULL;
    }

    if (out_len) *out_len = (int)strlen(buf);
    return buf;  // caller frees
}
esp_err_t http_get(const char *url, char *out_buf, size_t out_len, int timeout_ms)
{
    _LOG_I("URL request:%s",url);
    if (!url || !out_buf || out_len == 0) return ESP_ERR_INVALID_ARG;
    out_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 5000,
        .crt_bundle_attach = esp_crt_bundle_attach, // OK for HTTPS; harmless for HTTP
        // No event handler needed for simple GET
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        _LOG_E( "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    // Optional: fetch headers (also gives content length if provided)
    esp_http_client_fetch_headers(client);

    size_t total = 0;
    while (total + 1 < out_len) {
        int r = esp_http_client_read(client, out_buf + total, out_len - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    out_buf[total] = '\0';

    int status = esp_http_client_get_status_code(client);
    s_last_status=status;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        return ESP_OK;
    } else {
        _LOG_W( "GET %s -> HTTP %d", url, status);
        return ESP_FAIL;
    }
}
