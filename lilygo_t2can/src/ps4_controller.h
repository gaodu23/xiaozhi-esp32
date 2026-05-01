#pragma once
#include <Arduino.h>
#include "config.h"

// ---- PS4 DualShock 4 via Classic Bluetooth ----------------------------
// Library: ps4esp32 by aed3  (https://github.com/aed3/PS4-esp32)

void ps4_init();
bool ps4_connected();

// Normalised stick values: -128..127
int8_t ps4_lx();
int8_t ps4_ly();

// Buttons
bool ps4_cross();      // X  – speed down
bool ps4_square();     // □  – speed up
bool ps4_circle();     // ○  – horn
bool ps4_triangle();   // △  – switch mode
bool ps4_l1();         // L1 – left indicator
bool ps4_r1();         // R1 – right indicator
bool ps4_l2();         // L2 – hazard
bool ps4_r2();         // R2 – lights

// millis() timestamp of last received PS4 packet (0 if never)
uint32_t ps4_last_rx_ms();

// Wait up to timeout_ms for PS4 to connect; returns true if connected.
bool ps4_wait_connect(uint32_t timeout_ms = 10000);

// Select control method via button hold during startup.
// Waits up to 10 s: Cross = JSMerror, Square = FollowJSM.
// Returns selected method (default: METHOD_FOLLOW_JSM).
ControlMethod ps4_select_method();
