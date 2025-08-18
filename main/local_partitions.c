#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <stdio.h>
#include <string.h>

// Map app subtypes to a readable name (factory, ota_N, or hex)
static const char* app_subtype_str(esp_partition_subtype_t st, char *tmp, size_t n) {
    if (st == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
    if (st >= ESP_PARTITION_SUBTYPE_APP_OTA_0 && st <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {
        snprintf(tmp, n, "ota_%d", (int)(st - ESP_PARTITION_SUBTYPE_APP_OTA_0));
        return tmp;
    }
    snprintf(tmp, n, "subtype_0x%02x", (unsigned)st);
    return tmp;
}

// Re-entrant: caller supplies the buffer
static inline void boot_partition_string(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;

    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!boot) {
        snprintf(out, out_sz, "boot=? (esp_ota_get_boot_partition=NULL)");
        return;
    }

    char typebuf[16];
    const char *kind = app_subtype_str(boot->subtype, typebuf, sizeof(typebuf));
    const char *label = (boot->label && boot->label[0]) ? boot->label : "(no-label)";

    // address is 32-bit on ESP32; cast to unsigned to satisfy printf
    snprintf(out, out_sz, "boot=%s (label='%s', addr=0x%08x, size=%u)",
             kind, label, (unsigned)boot->address, (unsigned)boot->size);
}
static inline void running_partition_string(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;

    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!boot) {
        snprintf(out, out_sz, "boot=? (esp_ota_get_boot_partition=NULL)");
        return;
    }

    char typebuf[16];
    const char *kind = app_subtype_str(boot->subtype, typebuf, sizeof(typebuf));
    const char *label = (boot->label && boot->label[0]) ? boot->label : "(no-label)";

    // address is 32-bit on ESP32; cast to unsigned to satisfy printf
    snprintf(out, out_sz, "boot=%s (label='%s', addr=0x%08x, size=%u)",
             kind, label, (unsigned)boot->address, (unsigned)boot->size);
}

// Convenience: returns a static buffer (not thread-safe)
static inline const char* boot_partition_cstr(void) {
    static char s[96];
    boot_partition_string(s, sizeof(s));
    return s;
}
// Convenience: returns a static buffer (not thread-safe)
static inline const char* running_partition_cstr(void) {
    static char s[96];
    running_partition_string(s, sizeof(s));
    return s;
}



/* Usage:
   _LOG_I("Boot partition: %s", boot_partition_cstr());
   // or:
   char buf[96];
   boot_partition_string(buf, sizeof(buf));
   _LOG_I("Boot partition: %s", buf);
*/

