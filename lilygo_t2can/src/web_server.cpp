#include "web_server.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static AsyncWebServer s_server(80);
static AsyncWebSocket s_ws("/ws");

WebState g_web_state = {};

// Simple lock-free command queue (single producer, single consumer)
static volatile bool   s_cmd_ready = false;
static WebCommand      s_pending_cmd = {};

// ---------------------------------------------------------------------------

static void on_ws_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo *)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            // Null-terminate
            char buf[128] = {};
            size_t copy_len = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
            memcpy(buf, data, copy_len);

            // Parse simple JSON: {"cmd":"set_speed","value":75}
            StaticJsonDocument<128> doc;
            if (deserializeJson(doc, buf) == DeserializationError::Ok) {
                WebCommand cmd = {};
                cmd.valid = true;
                const char *c = doc["cmd"] | "";
                strncpy(cmd.cmd, c, sizeof(cmd.cmd) - 1);
                cmd.value = doc["value"] | 0;
                s_pending_cmd = cmd;
                s_cmd_ready = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------

void webserver_init() {
    // Mount LittleFS
    if (!LittleFS.begin(true)) {
        LOG("LittleFS mount failed");
    } else {
        LOG("LittleFS mounted");
    }

    // WiFi AP
    WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
    LOG("WiFi AP '%s' at %s", WIFI_SSID, WiFi.softAPIP().toString().c_str());

    // WebSocket
    s_ws.onEvent(on_ws_event);
    s_server.addHandler(&s_ws);

    // Serve static files from LittleFS
    s_server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // REST: GET /api/status — returns current state as JSON
    s_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        StaticJsonDocument<512> doc;
        doc["speed"]       = g_web_state.speed_pct;
        doc["ps4"]         = g_web_state.ps4_connected;
        doc["can"]         = g_web_state.can_connected;
        doc["battery"]     = g_web_state.battery_pct;
        doc["joy_x"]       = g_web_state.joy_x;
        doc["joy_y"]       = g_web_state.joy_y;
        doc["method"]      = g_web_state.method_name;
        doc["uptime_s"]    = g_web_state.uptime_s;

        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // REST: POST /api/time  {"unix":1234567890}  — sync clock from browser
    s_server.on("/api/time", HTTP_POST, [](AsyncWebServerRequest *req) {},
        nullptr,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len) == DeserializationError::Ok) {
                // Store unix timestamp offset if needed
                (void)doc["unix"];
            }
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    s_server.begin();
    LOG("HTTP server started");
}

void webserver_loop() {
    s_ws.cleanupClients();
}

void webserver_push_status() {
    if (s_ws.count() == 0) return;

    StaticJsonDocument<512> doc;
    doc["type"]     = "status";
    doc["speed"]    = g_web_state.speed_pct;
    doc["ps4"]      = g_web_state.ps4_connected;
    doc["can"]      = g_web_state.can_connected;
    doc["battery"]  = g_web_state.battery_pct;
    doc["joy_x"]    = g_web_state.joy_x;
    doc["joy_y"]    = g_web_state.joy_y;
    doc["method"]   = g_web_state.method_name;
    doc["uptime_s"] = g_web_state.uptime_s;

    // Battery history array
    JsonArray hist = doc.createNestedArray("batt_history");
    for (int i = 0; i < 60; i++) {
        hist.add(g_web_state.batt_history[(g_web_state.batt_history_idx + i) % 60]);
    }

    String out;
    serializeJson(doc, out);
    s_ws.textAll(out);
}

WebCommand webserver_poll_command() {
    if (!s_cmd_ready) return {false};
    s_cmd_ready = false;
    return s_pending_cmd;
}
