#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Actions for control API (enum { ..., *_MAX } style)
typedef enum {
  ACTION_START = 0,
  ACTION_CANCEL,
  ACTION_HITEMP,
  ACTION_UPDATE,
  ACTION_REBOOT,
  ACTION_MAX
} actions_t;

// Server lifecycle
void start_webserver(void);
void stop_webserver(void);

#ifdef __cplusplus
}
#endif
