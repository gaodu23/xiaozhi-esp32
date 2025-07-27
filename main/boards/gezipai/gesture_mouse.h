#ifndef GESTURE_MOUSE_H
#define GESTURE_MOUSE_H

#include <stdint.h>
#include <stdbool.h>
#include "qmi8658.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_hidd_prf_api.h"
#include "esp_gap_ble_api.h"
#include <esp_adc/adc_oneshot.h>

#ifdef __cplusplus
extern "C" {
#endif

// 体感鼠标参数
#define MOUSE_SENSITIVITY 0.1f
#define GYRO_THRESHOLD 0.8f
#define MOUSE_MAX_MOVE 10
#define MOUSE_SAMPLE_RATE 20

// BLE HID设备名称
#define HIDD_DEVICE_NAME "XiaoZhi-Mouse"

// 按键状态定义
typedef enum {
    BUTTON_STATE_IDLE = 0,
    BUTTON_STATE_LEFT = 1,
    BUTTON_STATE_RIGHT = 2,
    BUTTON_STATE_VOLUME_UP = 3,
    BUTTON_STATE_VOLUME_DOWN = 4
} button_state_t;

// 体感鼠标管理结构体
typedef struct {
    qmi8658_handle_t imu_sensor;
    bool initialized;
    bool ble_connected;
    uint16_t hid_conn_id;
    TaskHandle_t mouse_task_handle;
    TaskHandle_t hid_task_handle;
} gesture_mouse_t;

/**
 * @brief 初始化体感鼠标
 * @param mouse 体感鼠标句柄
 * @param i2c_bus I2C总线句柄
 * @param adc_handle ADC句柄（用于按键检测）
 * @return true 初始化成功，false 失败
 */
bool gesture_mouse_init(gesture_mouse_t *mouse, i2c_master_bus_handle_t i2c_bus, adc_oneshot_unit_handle_t adc_handle);

/**
 * @brief 启动体感鼠标服务
 * @param mouse 体感鼠标句柄
 * @return true 启动成功，false 失败
 */
bool gesture_mouse_start(gesture_mouse_t *mouse);

/**
 * @brief 停止体感鼠标服务
 * @param mouse 体感鼠标句柄
 */
void gesture_mouse_stop(gesture_mouse_t *mouse);

/**
 * @brief 释放体感鼠标资源
 * @param mouse 体感鼠标句柄
 */
void gesture_mouse_deinit(gesture_mouse_t *mouse);

#ifdef __cplusplus
}
#endif

#endif // GESTURE_MOUSE_H
