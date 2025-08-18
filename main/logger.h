#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize network mirroring of ESP_LOG output.
// You can enable UDP, TCP, or both on the same numeric port.
esp_err_t logger_init(const char *host, uint16_t port,int buffer);
esp_err_t logger_init_net(const char *host, uint16_t port, bool use_udp, bool use_tcp);

// Stop mirroring and restore the original vprintf.
void logger_shutdown_net(void);

// Optional: query drops (messages dropped due to full queue)
uint32_t logger_get_drop_count(void);

#ifdef __cplusplus
}
#endif
