#pragma once
#include <Arduino.h>
#include "driver/twai.h"
#include "config.h"

// ---- CAN / TWAI initialisation ----------------------------------------

bool rnet_can_init();
void rnet_can_stop();

// ---- Frame helpers ----------------------------------------------------

// Build a TWAI tx message from a 29-bit extended CAN ID and up to 8 data bytes.
twai_message_t rnet_build_frame(uint32_t ext_id, const uint8_t *data, uint8_t len);

// Build a joystick frame: x and y are signed -128..127 (PS4 stick values).
// Returns a ready-to-transmit TWAI message.
twai_message_t rnet_build_joy_frame(uint32_t joy_id, int8_t x, int8_t y);

// Send a pre-built frame.  Returns true on success.
bool rnet_send(const twai_message_t &msg, uint32_t timeout_ms = 5);

// Build-and-send shorthand using "cansend" text format (e.g. "0A040100#4B").
// Supports standard (3-char) and extended (8-char) IDs, hex data bytes.
bool rnet_cansend(const char *cansend_str, uint32_t timeout_ms = 5);

// ---- Receive helpers --------------------------------------------------

// Wait up to timeout_ms for any frame matching (id & mask) == (filter & mask).
// Returns true and fills *out if a matching frame arrives; false on timeout.
bool rnet_wait_frame(uint32_t filter, uint32_t mask,
                     twai_message_t *out, uint32_t timeout_ms);

// Non-blocking receive.  Returns true if a frame was available.
bool rnet_receive(twai_message_t *out, uint32_t timeout_ms = 0);

// ---- R-Net commands ---------------------------------------------------

void rnet_set_speed(uint8_t pct);          // 0–100 %
void rnet_horn_on();
void rnet_horn_off();
void rnet_indicator_left();
void rnet_indicator_right();
void rnet_hazards();
void rnet_lights();

// Send JSM error frames (3 × 0x0c000000#) to trigger JSM error state.
void rnet_induce_jsm_error();

// ---- Joystick helpers -------------------------------------------------

// Convert PS4 LStickX/Y (-128..127) to R-Net frame bytes (0x00–0xFF).
// R-Net joystick centre = 0x00; forward = positive Y.
uint8_t rnet_scale_axis(int8_t stick_val, bool invert);

// ---- Telemetry --------------------------------------------------------

struct RNetTelemetry {
    uint8_t  battery_pct;    // 0–100
    int16_t  motor_current;  // raw ADC from CAN, little-endian
    uint32_t last_joy_rx_ms; // millis() of last received JSM joystick frame
    uint32_t frames_sent;
    uint32_t frames_received;
    bool     can_ok;
};

extern RNetTelemetry g_telemetry;
void rnet_update_telemetry(const twai_message_t &frame);
