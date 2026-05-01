#pragma once

// ============================================================
// LilyGO T2-CAN GPIO pins — verify against board schematic!
// ============================================================
#define CAN_TX_PIN      GPIO_NUM_27
#define CAN_RX_PIN      GPIO_NUM_26

// WiFi AP
#define WIFI_SSID       "RNet-Controller"
#define WIFI_PASSWORD   "wheelchair"
#define WIFI_AP_IP      "192.168.4.1"

// PS4 MAC address — set this to your DualShock 4 MAC
#define PS4_MAC         "90:FB:A6:A6:FE:FA"

// R-Net protocol constants
#define RNET_BITRATE    125000        // 125 kbps
#define RNET_JOY_ID     0x02000100   // Joystick frame extended ID (slot 1)
#define RNET_JOY_MASK   0x0F000F00   // Mask to detect any joystick frame
#define RNET_JOY_BASE   0x02000100   // Base joystick ID pattern

#define RNET_SPEED_ID   0x0A040100   // Set max speed (extended)
#define RNET_HORN_ON    "0C040100#"
#define RNET_HORN_OFF   "0C040101#"
#define RNET_IND_LEFT   "0C040200#"  // Left indicator (placeholder - verify)
#define RNET_IND_RIGHT  "0C040300#"
#define RNET_HAZARD     "0C040400#"
#define RNET_LIGHTS     "0C040500#"

#define RNET_JSM_HEARTBEAT_ID  0x03C30F0F   // JSM heartbeat to detect before JSMerror
#define RNET_JSM_HB_MASK       0x1FFFFFFF

// CAN frame IDs for battery and motor current telemetry
#define RNET_BATTERY_ID  0x1C0C0100
#define RNET_BATTERY_MASK 0x0F0F0F00
#define RNET_MOTOR_ID    0x14300100

// Timing
#define JOY_FRAME_INTERVAL_MS    10    // R-Net joystick frame period
#define FOLLOW_JSM_TIMEOUT_MS    50    // Max wait for JSM frame before sending anyway
#define WATCHDOG_TIMEOUT_MS    3000    // Stop chair if no PS4 input for 3s
#define HEARTBEAT_INTERVAL_MS   200    // Programmer heartbeat
#define WS_BROADCAST_INTERVAL_MS 100  // WebSocket status push interval

// Default speed on startup (0–100 %)
#define DEFAULT_SPEED_PCT  75

// NVS namespace for persistent settings
#define NVS_NAMESPACE   "rnet"

// Control method enum
enum ControlMethod {
    METHOD_NONE       = 0,
    METHOD_FOLLOW_JSM = 1,
    METHOD_JSM_ERROR  = 2,
};

// Logging helper
#define LOG(fmt, ...) Serial.printf("[%6lu] " fmt "\n", millis(), ##__VA_ARGS__)
