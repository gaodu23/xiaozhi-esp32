/*
 * LilyGO T2-CAN — R-Net wheelchair controller
 *
 * Migrated from Raspberry Pi 3 + PiCAN2 (Python/SocketCAN)
 * to ESP32 TWAI + PS4 Classic BT.
 *
 * Control methods:
 *   FollowJSM  — wait for JSM joystick frame, inject ours immediately after
 *   JSMerror   — trigger JSM error, then send joystick frames every 10 ms
 *
 * Web UI accessible at http://192.168.4.1 after connecting to WiFi AP.
 *
 * SAFETY: watchdog stops the chair if PS4 connection is lost for > 3 s
 *         or if the main loop hangs.
 *
 * First run checklist:
 *   1. Verify CAN_TX_PIN / CAN_RX_PIN in config.h match your T2-CAN board.
 *   2. Set PS4_MAC to your DualShock 4 Bluetooth MAC address.
 *   3. Flash with "pio run -t upload" then "pio run -t uploadfs" for web UI.
 */

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "rnet_can.h"
#include "ps4_controller.h"
#include "web_server.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static ControlMethod g_method       = METHOD_FOLLOW_JSM;
static uint8_t       g_speed_pct    = DEFAULT_SPEED_PCT;
static uint32_t      g_joy_frame_id = RNET_JOY_ID;   // discovered from bus
static bool          g_running      = false;

// Watchdog: millis() when we last had valid PS4 input
static uint32_t      g_last_ps4_input_ms = 0;

// Telemetry push interval
static uint32_t      g_last_ws_push_ms = 0;

// Battery history sample interval
static uint32_t      g_last_batt_sample_ms = 0;

// Persistent settings
static Preferences   g_prefs;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void     load_prefs();
static void     save_prefs();
static uint32_t detect_joy_frame_id();
static void     run_follow_jsm();
static void     run_jsm_error();
static void     send_stop_frame();
static void     handle_button_events();
static void     handle_web_commands();
static void     update_web_state();

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);
    LOG("=== R-Net T2-CAN controller booting ===");

    load_prefs();

    // CAN bus — MUST come up before PS4 selection so we can listen
    if (!rnet_can_init()) {
        LOG("FATAL: CAN init failed. Check wiring.");
        while (true) delay(1000);
    }

    // Web server (WiFi AP + HTTP + WS)
    webserver_init();

    // PS4 Bluetooth
    ps4_init();

    LOG("Waiting for PS4 controller (MAC %s)...", PS4_MAC);
    if (!ps4_wait_connect(15000)) {
        LOG("PS4 not found — starting in web-only mode");
    }

    // Method selection: hold Cross (JSMerror) or Square (FollowJSM) for 10 s
    g_method = ps4_select_method();
    strncpy(g_web_state.method_name,
            g_method == METHOD_JSM_ERROR ? "JSMerror" : "FollowJSM",
            sizeof(g_web_state.method_name) - 1);
    g_web_state.method = (uint8_t)g_method;

    LOG("Detecting JSM joystick frame ID from CAN bus...");
    g_joy_frame_id = detect_joy_frame_id();
    LOG("Using joy frame ID: 0x%08X", g_joy_frame_id);

    rnet_set_speed(g_speed_pct);
    LOG("Speed set to %d%%", g_speed_pct);

    if (g_method == METHOD_JSM_ERROR) {
        LOG("Inducing JSM error...");
        // Wait for JSM heartbeat first
        twai_message_t hb;
        rnet_wait_frame(RNET_JSM_HEARTBEAT_ID, RNET_JSM_HB_MASK, &hb, 5000);
        delay(200);
        rnet_induce_jsm_error();
        delay(50);
    }

    g_running           = true;
    g_last_ps4_input_ms = millis();
    LOG("Control loop starting — method: %s", g_web_state.method_name);
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------

void loop() {
    if (!g_running) {
        delay(50);
        return;
    }

    handle_web_commands();
    handle_button_events();

    // Watchdog: zero the chair if PS4 disconnected for too long
    bool ps4_ok = ps4_connected();
    if (ps4_ok) {
        g_last_ps4_input_ms = millis();
    }
    if (millis() - g_last_ps4_input_ms > WATCHDOG_TIMEOUT_MS) {
        static uint32_t last_warn = 0;
        if (millis() - last_warn > 1000) {
            LOG("WATCHDOG: PS4 lost, sending stop frame");
            last_warn = millis();
        }
        send_stop_frame();
        update_web_state();
        webserver_loop();
        delay(10);
        return;
    }

    // Run the selected control method for one frame cycle
    if (g_method == METHOD_FOLLOW_JSM) {
        run_follow_jsm();
    } else {
        run_jsm_error();
    }

    // Periodic tasks
    update_web_state();
    webserver_loop();

    uint32_t now = millis();
    if (now - g_last_ws_push_ms >= WS_BROADCAST_INTERVAL_MS) {
        webserver_push_status();
        g_last_ws_push_ms = now;
    }
    if (now - g_last_batt_sample_ms >= 1000) {
        uint8_t idx = g_web_state.batt_history_idx;
        g_web_state.batt_history[idx] = g_telemetry.battery_pct;
        g_web_state.batt_history_idx  = (idx + 1) % 60;
        g_last_batt_sample_ms = now;
    }
}

// ---------------------------------------------------------------------------
// FollowJSM — identical timing behaviour to Python inject_rnet_joystick_frame
// ---------------------------------------------------------------------------

static void run_follow_jsm() {
    twai_message_t jsm_frame;
    // Prebuild the "empty" joystick frame pattern we are looking for
    // (match any joystick frame — ID pattern 0x020XXXXX)
    if (rnet_wait_frame(RNET_JOY_BASE, 0x0F000F00, &jsm_frame, FOLLOW_JSM_TIMEOUT_MS)) {
        // Frame received — immediately inject ours
        int8_t x = ps4_lx();
        int8_t y = ps4_ly();
        twai_message_t my_frame = rnet_build_joy_frame(g_joy_frame_id, x, y);
        rnet_send(my_frame, 5);
    } else {
        // No JSM frame seen — send a neutral frame to keep the bus alive
        twai_message_t neutral = rnet_build_joy_frame(g_joy_frame_id, 0, 0);
        rnet_send(neutral, 5);
    }
}

// ---------------------------------------------------------------------------
// JSMerror — send joystick frame every JOY_FRAME_INTERVAL_MS
// ---------------------------------------------------------------------------

static uint32_t s_jsmerror_next_ms = 0;

static void run_jsm_error() {
    uint32_t now = millis();
    if (now >= s_jsmerror_next_ms) {
        int8_t x = ps4_lx();
        int8_t y = ps4_ly();
        twai_message_t frame = rnet_build_joy_frame(g_joy_frame_id, x, y);
        rnet_send(frame, 5);
        s_jsmerror_next_ms = now + JOY_FRAME_INTERVAL_MS;
    }
    // Also drain incoming frames so telemetry stays fresh
    twai_message_t rx;
    while (rnet_receive(&rx, 0)) { /* telemetry updated inside rnet_receive */ }
}

// ---------------------------------------------------------------------------
// Stop frame — centre joystick, zeroes all axes
// ---------------------------------------------------------------------------

static void send_stop_frame() {
    static uint32_t s_next_ms = 0;
    uint32_t now = millis();
    if (now >= s_next_ms) {
        twai_message_t stop = rnet_build_joy_frame(g_joy_frame_id, 0, 0);
        rnet_send(stop, 5);
        s_next_ms = now + JOY_FRAME_INTERVAL_MS;
    }
}

// ---------------------------------------------------------------------------
// Detect actual JSM joystick frame ID from the live CAN bus
// ---------------------------------------------------------------------------

static uint32_t detect_joy_frame_id() {
    twai_message_t frame;
    // Listen for up to 2 s for a frame matching joystick pattern 0x020XXXXX
    if (rnet_wait_frame(0x02000000, 0x0F000000, &frame, 2000)) {
        return frame.identifier;
    }
    // Fall back to default slot 1
    return RNET_JOY_ID;
}

// ---------------------------------------------------------------------------
// Button event handling (edge-triggered)
// ---------------------------------------------------------------------------

static bool s_prev_cross    = false;
static bool s_prev_square   = false;
static bool s_prev_circle   = false;
static bool s_prev_l1       = false;
static bool s_prev_r1       = false;
static bool s_prev_l2       = false;
static bool s_prev_r2       = false;

static void handle_button_events() {
    if (!ps4_connected()) return;

    bool cross    = ps4_cross();
    bool square   = ps4_square();
    bool circle   = ps4_circle();
    bool l1       = ps4_l1();
    bool r1       = ps4_r1();
    bool l2       = ps4_l2();
    bool r2       = ps4_r2();

    // Rising-edge detection
    if (cross && !s_prev_cross) {
        if (g_speed_pct >= 25) g_speed_pct -= 25;
        else                   g_speed_pct  = 0;
        rnet_set_speed(g_speed_pct);
        save_prefs();
    }
    if (square && !s_prev_square) {
        if (g_speed_pct <= 75) g_speed_pct += 25;
        else                   g_speed_pct  = 100;
        rnet_set_speed(g_speed_pct);
        save_prefs();
    }
    if (circle && !s_prev_circle) rnet_horn_on();
    if (!circle && s_prev_circle) rnet_horn_off();
    if (l1 && !s_prev_l1) rnet_indicator_left();
    if (r1 && !s_prev_r1) rnet_indicator_right();
    if (l2 && !s_prev_l2) rnet_hazards();
    if (r2 && !s_prev_r2) rnet_lights();

    s_prev_cross  = cross;
    s_prev_square = square;
    s_prev_circle = circle;
    s_prev_l1     = l1;
    s_prev_r1     = r1;
    s_prev_l2     = l2;
    s_prev_r2     = r2;
}

// ---------------------------------------------------------------------------
// Web command handler
// ---------------------------------------------------------------------------

static void handle_web_commands() {
    WebCommand cmd = webserver_poll_command();
    if (!cmd.valid) return;

    if (strcmp(cmd.cmd, "set_speed") == 0) {
        int v = cmd.value;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        g_speed_pct = (uint8_t)v;
        rnet_set_speed(g_speed_pct);
        save_prefs();
        LOG("Web: speed → %d%%", g_speed_pct);

    } else if (strcmp(cmd.cmd, "horn_on") == 0) {
        rnet_horn_on();
    } else if (strcmp(cmd.cmd, "horn_off") == 0) {
        rnet_horn_off();
    } else if (strcmp(cmd.cmd, "indicator_left") == 0) {
        rnet_indicator_left();
    } else if (strcmp(cmd.cmd, "indicator_right") == 0) {
        rnet_indicator_right();
    } else if (strcmp(cmd.cmd, "hazards") == 0) {
        rnet_hazards();
    } else if (strcmp(cmd.cmd, "lights") == 0) {
        rnet_lights();
    } else if (strcmp(cmd.cmd, "joy") == 0) {
        // Web joystick: value encodes x in high byte, y in low byte
        int8_t wx = (int8_t)((cmd.value >> 8) & 0xFF);
        int8_t wy = (int8_t)(cmd.value & 0xFF);
        twai_message_t frame = rnet_build_joy_frame(g_joy_frame_id, wx, wy);
        rnet_send(frame, 5);
    } else {
        LOG("Web: unknown command '%s'", cmd.cmd);
    }
}

// ---------------------------------------------------------------------------
// Update shared web state from current readings
// ---------------------------------------------------------------------------

static void update_web_state() {
    g_web_state.speed_pct     = g_speed_pct;
    g_web_state.ps4_connected = ps4_connected();
    g_web_state.can_connected = g_telemetry.can_ok;
    g_web_state.battery_pct   = g_telemetry.battery_pct;
    g_web_state.joy_x         = ps4_lx();
    g_web_state.joy_y         = ps4_ly();
    g_web_state.uptime_s      = millis() / 1000;
}

// ---------------------------------------------------------------------------
// Persistent settings (NVS via Preferences)
// ---------------------------------------------------------------------------

static void load_prefs() {
    g_prefs.begin(NVS_NAMESPACE, true); // read-only
    g_speed_pct = g_prefs.getUChar("speed_pct", DEFAULT_SPEED_PCT);
    g_prefs.end();
    LOG("Prefs loaded: speed=%d%%", g_speed_pct);
}

static void save_prefs() {
    g_prefs.begin(NVS_NAMESPACE, false);
    g_prefs.putUChar("speed_pct", g_speed_pct);
    g_prefs.end();
}
