#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <stdio.h>
#include <string.h>

static inline void boot_partition_string(char *out, size_t out_sz);
static inline const char* boot_partition_cstr(void);
static inline void running_partition_string(char *out, size_t out_sz);
static inline const char* running_partition_cstr(void);
