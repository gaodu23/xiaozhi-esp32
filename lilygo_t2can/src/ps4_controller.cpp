#include "ps4_controller.h"
#include <PS4Controller.h>

static uint32_t s_last_rx_ms = 0;

void ps4_init() {
    PS4.begin(PS4_MAC);
    LOG("PS4 BT init, waiting for MAC %s", PS4_MAC);
}

bool ps4_connected() {
    return PS4.isConnected();
}

int8_t ps4_lx() {
    return ps4_connected() ? (int8_t)PS4.LStickX() : 0;
}

int8_t ps4_ly() {
    return ps4_connected() ? (int8_t)PS4.LStickY() : 0;
}

bool ps4_cross()    { return ps4_connected() && PS4.Cross(); }
bool ps4_square()   { return ps4_connected() && PS4.Square(); }
bool ps4_circle()   { return ps4_connected() && PS4.Circle(); }
bool ps4_triangle() { return ps4_connected() && PS4.Triangle(); }
bool ps4_l1()       { return ps4_connected() && PS4.L1(); }
bool ps4_r1()       { return ps4_connected() && PS4.R1(); }
bool ps4_l2()       { return ps4_connected() && PS4.L2(); }
bool ps4_r2()       { return ps4_connected() && PS4.R2(); }

uint32_t ps4_last_rx_ms() {
    return s_last_rx_ms;
}

bool ps4_wait_connect(uint32_t timeout_ms) {
    uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        if (PS4.isConnected()) {
            LOG("PS4 connected");
            return true;
        }
        delay(100);
    }
    LOG("PS4 not connected after %lu ms", timeout_ms);
    return false;
}

ControlMethod ps4_select_method() {
    LOG("Hold Cross=JSMerror, Square=FollowJSM (10s to choose)");

    uint32_t deadline = millis() + 10000;
    while (millis() < deadline) {
        if (!PS4.isConnected()) {
            delay(50);
            continue;
        }
        s_last_rx_ms = millis();

        if (PS4.Cross()) {
            LOG("Method selected: JSMerror");
            return METHOD_JSM_ERROR;
        }
        if (PS4.Square()) {
            LOG("Method selected: FollowJSM");
            return METHOD_FOLLOW_JSM;
        }
        delay(50);
    }
    LOG("No selection — defaulting to FollowJSM");
    return METHOD_FOLLOW_JSM;
}
