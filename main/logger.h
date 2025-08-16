#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize TCP logger.
 * 
 * @param host Remote server IP or hostname
 * @param port Remote TCP port
 * @param buffer_size Size of ring buffer for storing log messages
 * @return true on success, false otherwise
 */
bool logger_init(const char *host, uint16_t port, size_t buffer_size);

/**
 * @brief Stop TCP logger and free resources.
 */
void logger_stop(void);

/**
 * @brief Force flush of buffered logs to remote server.
 */
void logger_flush(void);

#ifdef __cplusplus
}
#endif

#endif // LOGGER_H
