#include "rnet_can.h"
#include <string.h>
#include <stdlib.h>

RNetTelemetry g_telemetry = {};

// ---------------------------------------------------------------------------

bool rnet_can_init() {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_125KBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    g.rx_queue_len = 32;
    g.tx_queue_len = 16;

    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        LOG("TWAI driver install failed");
        return false;
    }
    if (twai_start() != ESP_OK) {
        LOG("TWAI start failed");
        return false;
    }
    g_telemetry.can_ok = true;
    LOG("TWAI started on TX=%d RX=%d @ 125kbps", CAN_TX_PIN, CAN_RX_PIN);
    return true;
}

void rnet_can_stop() {
    twai_stop();
    twai_driver_uninstall();
    g_telemetry.can_ok = false;
}

// ---------------------------------------------------------------------------

twai_message_t rnet_build_frame(uint32_t ext_id, const uint8_t *data, uint8_t len) {
    twai_message_t msg = {};
    msg.extd           = 1;           // extended 29-bit ID
    msg.identifier     = ext_id & 0x1FFFFFFF;
    msg.data_length_code = (len > 8) ? 8 : len;
    if (data && len) {
        memcpy(msg.data, data, msg.data_length_code);
    }
    return msg;
}

twai_message_t rnet_build_joy_frame(uint32_t joy_id, int8_t x, int8_t y) {
    uint8_t data[4] = {
        rnet_scale_axis(x, false),
        rnet_scale_axis(y, true),   // Y inverted: forward stick = positive R-Net
        0x00,
        0x00
    };
    return rnet_build_frame(joy_id, data, 4);
}

bool rnet_send(const twai_message_t &msg, uint32_t timeout_ms) {
    esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        g_telemetry.frames_sent++;
        return true;
    }
    return false;
}

// Parse "cansend" format string: XXXXXXXX#YYYYYY  (8-char ext or 3-char std ID)
bool rnet_cansend(const char *s, uint32_t timeout_ms) {
    const char *hash = strchr(s, '#');
    if (!hash) return false;

    size_t id_len = hash - s;
    char id_buf[9] = {};
    memcpy(id_buf, s, id_len < 8 ? id_len : 8);

    uint32_t can_id = strtoul(id_buf, nullptr, 16);

    const char *data_str = hash + 1;
    uint8_t data[8] = {};
    uint8_t data_len = 0;

    // Parse pairs of hex chars
    while (*data_str && *(data_str+1) && data_len < 8) {
        char byte_str[3] = {data_str[0], data_str[1], 0};
        data[data_len++] = (uint8_t)strtoul(byte_str, nullptr, 16);
        data_str += 2;
        if (*data_str == '.') data_str++; // allow dot separators
    }

    bool is_ext = (id_len == 8);
    twai_message_t msg = {};
    msg.extd           = is_ext ? 1 : 0;
    msg.identifier     = can_id;
    msg.data_length_code = data_len;
    memcpy(msg.data, data, data_len);

    return rnet_send(msg, timeout_ms);
}

// ---------------------------------------------------------------------------

bool rnet_wait_frame(uint32_t filter, uint32_t mask,
                     twai_message_t *out, uint32_t timeout_ms) {
    uint32_t deadline = millis() + timeout_ms;
    twai_message_t msg;
    while (millis() < deadline) {
        uint32_t remaining = deadline - millis();
        if (twai_receive(&msg, pdMS_TO_TICKS(remaining > 0 ? remaining : 1)) == ESP_OK) {
            g_telemetry.frames_received++;
            rnet_update_telemetry(msg);
            if ((msg.identifier & mask) == (filter & mask)) {
                if (out) *out = msg;
                return true;
            }
        }
    }
    return false;
}

bool rnet_receive(twai_message_t *out, uint32_t timeout_ms) {
    if (twai_receive(out, pdMS_TO_TICKS(timeout_ms)) == ESP_OK) {
        g_telemetry.frames_received++;
        rnet_update_telemetry(*out);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------

void rnet_set_speed(uint8_t pct) {
    if (pct > 100) pct = 100;
    char buf[20];
    snprintf(buf, sizeof(buf), "0A040100#%02X", pct);
    rnet_cansend(buf);
    LOG("Speed set to %d%%", pct);
}

void rnet_horn_on()         { rnet_cansend(RNET_HORN_ON); }
void rnet_horn_off()        { rnet_cansend(RNET_HORN_OFF); }
void rnet_indicator_left()  { rnet_cansend(RNET_IND_LEFT); }
void rnet_indicator_right() { rnet_cansend(RNET_IND_RIGHT); }
void rnet_hazards()         { rnet_cansend(RNET_HAZARD); }
void rnet_lights()          { rnet_cansend(RNET_LIGHTS); }

void rnet_induce_jsm_error() {
    for (int i = 0; i < 3; i++) {
        rnet_cansend("0C000000#");
        delay(1);
    }
    LOG("JSMerror: 3x 0C000000# sent");
}

// ---------------------------------------------------------------------------

uint8_t rnet_scale_axis(int8_t stick_val, bool invert) {
    // PS4 stick: -128..127 → R-Net: 0x00–0xFF (centre = 0x00, not 0x80)
    // R-Net encodes as signed byte in a uint8 field: positive = forward/right
    int v = stick_val;
    if (invert) v = -v;
    // Dead-zone: ignore small values
    if (v > -8 && v < 8) return 0x00;
    // Map -128..127 to 0x81..0xFF (negative) and 0x01..0x7F (positive)
    // which is how R-Net stores signed joystick: cast to uint8 directly.
    return (uint8_t)(int8_t)v;
}

// ---------------------------------------------------------------------------

void rnet_update_telemetry(const twai_message_t &f) {
    uint32_t id = f.identifier;

    // Battery level: 1C0C0X00# Xx  (byte 0 = 0x00–0x64)
    if ((id & 0x0F0F0F00) == (RNET_BATTERY_ID & 0x0F0F0F00) && f.data_length_code >= 1) {
        g_telemetry.battery_pct = f.data[0];
    }

    // Motor current: 14300X00# LlHh  (little-endian int16)
    if ((id & 0x0FFFF000) == 0x14300000 && f.data_length_code >= 2) {
        g_telemetry.motor_current = (int16_t)(f.data[0] | (f.data[1] << 8));
    }

    // Joystick received
    if ((id & 0x0F000F00) == (RNET_JOY_BASE & 0x0F000F00)) {
        g_telemetry.last_joy_rx_ms = millis();
    }
}
