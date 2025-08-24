#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// ──────────────────────────────────────────────────────────────────────────────
// Actions API (grouped) — uses your explicit enum and an ACTIONS_TABLE mapping
// so the UI/routes/dispatcher stay in sync. If you edit the enum, update the
// ACTIONS_TABLE macro below to match token names and desired GROUP/NAME labels.
// ──────────────────────────────────────────────────────────────────────────────

// Actions for control API (enum { ..., *_MAX } style)
typedef enum {
  // Run cycles
  ACTION_CYCLE_NORMAL,
  ACTION_CYCLE_TESTER,
  ACTION_CYCLE_HITEMP,
  // DO actions
  ACTION_DO_PAUSE,
  ACTION_DO_RESUME,
  // Toggle specific device(s)
  ACTION_TOGGLE_DRAIN,
  ACTION_TOGGLE_FILL,
  ACTION_TOGGLE_SPRAY,
  ACTION_TOGGLE_HEAT,
  ACTION_TOGGLE_SOAP,
  ACTION_TOGGLE_LEDS,
  // Admin actions
  ACTION_ADMIN_CANCEL,
  ACTION_ADMIN_FIRMWAREUPDATE,
  ACTION_ADMIN_REBOOT,
  ACTION_ADMIN_SKIP_STEP,

  ACTION_MAX
} actions_t;

// Mapping used to generate UI groups, routes and dispatch switch
#ifndef ACTIONS_TABLE
#define ACTIONS_TABLE(XX)                                                          \
    /* GROUP ,   NAME         , TOKEN */                                           \
    XX(CYCLE , NORMAL         , ACTION_CYCLE_NORMAL)                               \
    XX(CYCLE , TESTER         , ACTION_CYCLE_TESTER)                               \
    XX(CYCLE , HITEMP         , ACTION_CYCLE_HITEMP)                               \
    XX(DO    , PAUSE          , ACTION_DO_PAUSE)                                   \
    XX(DO    , RESUME         , ACTION_DO_RESUME)                                   \
    XX(TOGGLE, DRAIN          , ACTION_TOGGLE_DRAIN)                               \
    XX(TOGGLE, FILL           , ACTION_TOGGLE_FILL)                                \
    XX(TOGGLE, SPRAY          , ACTION_TOGGLE_SPRAY)                               \
    XX(TOGGLE, HEAT           , ACTION_TOGGLE_HEAT)                                \
    XX(TOGGLE, SOAP           , ACTION_TOGGLE_SOAP)                                \
    XX(TOGGLE, LEDS           , ACTION_TOGGLE_LEDS)                                \
    XX(ADMIN , CANCEL         , ACTION_ADMIN_CANCEL)                               \
    XX(ADMIN , FIRMWAREUPDATE , ACTION_ADMIN_FIRMWAREUPDATE)                       \
    XX(ADMIN , REBOOT         , ACTION_ADMIN_REBOOT)                               \
    XX(ADMIN , SKIP_STEP      , ACTION_ADMIN_SKIP_STEP)
#endif

// Server lifecycle
void start_webserver(void);
void stop_webserver(void);

// Utility so other modules can query
bool http_server_is_running(void);

#ifdef __cplusplus
}
#endif
