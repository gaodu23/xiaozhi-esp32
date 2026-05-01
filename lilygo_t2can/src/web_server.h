#pragma once
#include <Arduino.h>
#include "config.h"

// ---- Web server + WebSocket ------------------------------------------
// Serves the single-page UI from LittleFS /data/index.html.
// Push-updates status to all connected browsers via WebSocket.

void webserver_init();

// Call from main loop — dispatches any queued WebSocket frames.
void webserver_loop();

// Push current telemetry + control state to all WS clients.
void webserver_push_status();

// ---- Shared state exposed to web layer --------------------------------

struct WebState {
    uint8_t  speed_pct;       // current max speed setting (0–100)
    bool     ps4_connected;
    bool     can_connected;
    uint8_t  battery_pct;
    int8_t   joy_x;
    int8_t   joy_y;
    uint8_t  method;          // ControlMethod enum value
    char     method_name[16];
    uint32_t uptime_s;
    // Telemetry ring buffer (last 60 samples, 1/s)
    uint8_t  batt_history[60];
    uint8_t  batt_history_idx;
};

extern WebState g_web_state;

// Commands received from browser via WebSocket:
struct WebCommand {
    bool     valid;
    char     cmd[32];
    int      value;
};

// Non-blocking: returns a pending command if one arrived, or {valid=false}.
WebCommand webserver_poll_command();
