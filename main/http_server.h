#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Actions accepted by the control API.
// Keep *_MAX last for easy iteration/array sizing.
typedef enum {
  ACTION_START = 0,
  ACTION_CANCEL,
  ACTION_HITEMP,
  ACTION_UPDATE,
  ACTION_REBOOT,
  ACTION_MAX
} actions_t;

// Start/stop the embedded webserver (idempotent start).
void start_webserver(void);
void stop_webserver(void);

#ifdef __cplusplus
}
#endif
