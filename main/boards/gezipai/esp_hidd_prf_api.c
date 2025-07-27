#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_hidd_prf_api.h"
#include "hid_dev.h"
#include "hidd_le_prf_int.h"
#include "esp_gatts_api.h"

// 取消之前包含文件中的TAG定义
#undef TAG
#define TAG "ESP_HIDD_API"

// HID设备回调函数
static esp_hidd_event_cb_t hidd_cb = NULL;

/**
 * @brief 获取活动连接的连接ID
 */
static uint16_t get_active_conn_id(void) {
    for (int i = 0; i < HID_MAX_APPS; i++) {
        if (hidd_le_env.hidd_clcb[i].in_use && hidd_le_env.hidd_clcb[i].connected) {
            return hidd_le_env.hidd_clcb[i].conn_id;
        }
    }
    return 0; // 没有活动连接
}

/**
 * @brief 初始化HID设备配置文件
 */
esp_err_t esp_ble_hidd_profile_init(void) {
    ESP_LOGI(TAG, "HID device profile initializing...");
    
    if (hidd_le_env.enabled) {
        ESP_LOGE(TAG, "HID device profile already initialized");
        return ESP_FAIL;
    }
    
    // 初始化HID环境
    memset(&hidd_le_env, 0, sizeof(hidd_le_env_t));
    hidd_le_env.enabled = true;
    ESP_LOGI(TAG, "HID environment reset and enabled");
    
    return ESP_OK;
}

/**
 * @brief 反初始化HID设备配置文件
 */
esp_err_t esp_ble_hidd_profile_deinit(void) {
    ESP_LOGI(TAG, "HID device profile deinitializing...");
    
    hidd_cb = NULL;
    
    return ESP_OK;
}

/**
 * @brief 获取HIDD配置文件版本
 */
uint16_t esp_hidd_get_version(void) {
    return 0x0100; // Version 1.0
}

/**
 * @brief 注册HID设备回调
 */
esp_err_t esp_ble_hidd_register_callbacks(esp_hidd_event_cb_t callbacks) {
    if (callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Registering HID device callbacks...");
    
    // 设置回调函数到HID LE环境中
    hidd_le_env.hidd_cb = callbacks;
    ESP_LOGI(TAG, "HID device callbacks set");
    
    // 注册GATT回调
    extern esp_err_t hidd_register_cb(void);
    esp_err_t ret = hidd_register_cb();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GATT callbacks: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "GATT callbacks registered successfully");
    
    // 注册电池服务应用程序
    ret = esp_ble_gatts_app_register(BATTRAY_APP_ID);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register battery app: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Battery application registration initiated");
    
    // 注册HID应用程序
    ret = esp_ble_gatts_app_register(HIDD_APP_ID);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register HID app: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "HID application registration initiated");
    
    return ESP_OK;
}

/**
 * @brief 发送HID报告
 */
esp_err_t esp_hidd_send_report(uint8_t map_index, uint8_t report_id, 
                               uint8_t report_type, uint8_t *report_data, 
                               uint8_t report_size) {
    if (report_data == NULL || report_size == 0) {
        ESP_LOGE(TAG, "Invalid report data");
        return ESP_ERR_INVALID_ARG;
    }
    
    uint16_t conn_id = get_active_conn_id();
    if (conn_id == 0) {
        ESP_LOGW(TAG, "No active connection");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 调用HID设备发送报告函数
    hid_dev_send_report(hidd_le_env.gatt_if, conn_id, report_id, 
                       report_type, report_size, report_data);
    
    ESP_LOGD(TAG, "HID report sent: id=%d, type=%d, size=%d", 
             report_id, report_type, report_size);
    
    return ESP_OK;
}

/**
 * @brief 触发HID事件回调
 */
void esp_hidd_cb_event_handler(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param) {
    if (hidd_cb != NULL) {
        hidd_cb(event, param);
    }
}

/**
 * @brief 发送消费者控制命令
 */
esp_err_t esp_hidd_send_consumer_value(uint16_t conn_id, uint8_t key_cmd, bool key_pressed) {
    uint8_t buffer[2] = {0};
    
    if (key_pressed) {
        hid_consumer_build_report(buffer, (consumer_cmd_t)key_cmd);
    }
    
    return esp_hidd_send_report(0, 1, HID_TYPE_INPUT, buffer, 2);
}

/**
 * @brief 发送鼠标数据
 */
void esp_hidd_send_mouse_value(uint16_t conn_id, uint8_t mouse_button, int8_t mickeys_x, int8_t mickeys_y) {
    uint8_t buffer[4] = {0};
    
    buffer[0] = mouse_button;
    buffer[1] = mickeys_x;
    buffer[2] = mickeys_y;
    buffer[3] = 0;  // wheel
    
    esp_hidd_send_report(0, 0, HID_TYPE_INPUT, buffer, 4);
}
