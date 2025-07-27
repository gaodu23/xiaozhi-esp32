#include "gesture_mouse.h"

// ADC按键检测相关定义
#define GESTURE_BUTTON_ADC_CHANNEL ADC_CHANNEL_8  // GPIO9对应ADC1_CH8
#define GESTURE_BUTTON_ADC_ATTEN ADC_ATTEN_DB_12
#define BUTTON_DEBOUNCE_TIME 50
#define ADC_SAMPLES 5

// ADC按键阈值定义（根据分压电路调整）
#define BUTTON_IDLE_MIN 3900
#define BUTTON_UP_MAX 200
#define BUTTON_RIGHT_MIN 200
#define BUTTON_RIGHT_MAX 600  
#define BUTTON_LEFT_MIN 1600
#define BUTTON_LEFT_MAX 2100
#define BUTTON_DOWN_MIN 2400
#define BUTTON_DOWN_MAX 2900

// 全局变量
static gesture_mouse_t* g_mouse = NULL;
static adc_oneshot_unit_handle_t g_adc_handle = NULL;
static bool sec_conn = false;

#include "esp_hidd_prf_api.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "hid_dev.h"
#include "config.h"
#include <math.h>
#include <esp_adc/adc_oneshot.h>
#include <string.h>

// 取消HID设备profile中的TAG定义，使用我们自己的TAG
#undef TAG
#define TAG "GestureMouse"

// 前向声明
static esp_err_t init_adc_button(adc_oneshot_unit_handle_t adc_handle);
static button_state_t read_button_state(void);

// HID服务UUID
static uint8_t hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

// BLE广播数据
static esp_ble_adv_data_t hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x03C2,           // HID鼠标设备
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid = hidd_service_uuid128,
    .flag = 0x6,
};

// BLE广播参数
static esp_ble_adv_params_t hidd_adv_params = {
    .adv_int_min = 0x100,  // 增加广播间隔以改善与WiFi的共存
    .adv_int_max = 0x200,  // 增加广播间隔以改善与WiFi的共存
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// 内部函数声明
static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param);
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void mouse_sensor_task(void *pvParameters);
static void hid_demo_task(void *pvParameters);
static float constrain(float value, float min_val, float max_val);

/**
 * @brief 初始化体感鼠标
 */
bool gesture_mouse_init(gesture_mouse_t *mouse, i2c_master_bus_handle_t i2c_bus, adc_oneshot_unit_handle_t adc_handle) {
    if (mouse == NULL) {
        ESP_LOGE(TAG, "鼠标句柄为NULL");
        return false;
    }

    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "ADC句柄为NULL");
        return false;
    }

    memset(mouse, 0, sizeof(gesture_mouse_t));
    g_mouse = mouse;

    // 初始化IMU传感器
    if (!qmi8658_init(&mouse->imu_sensor, i2c_bus, QMI8658_ADDRESS_LOW)) {
        ESP_LOGE(TAG, "IMU传感器初始化失败");
        return false;
    }

    // 配置传感器
    qmi8658_set_gyro_range(&mouse->imu_sensor, QMI8658_GYRO_RANGE_512DPS);
    qmi8658_set_gyro_odr(&mouse->imu_sensor, QMI8658_GYRO_ODR_125HZ);
    qmi8658_set_accel_range(&mouse->imu_sensor, QMI8658_ACCEL_RANGE_2G);
    qmi8658_set_accel_odr(&mouse->imu_sensor, QMI8658_ACCEL_ODR_125HZ);
    qmi8658_enable_sensors(&mouse->imu_sensor, QMI8658_ENABLE_GYRO | QMI8658_ENABLE_ACCEL);
    
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // 初始化ADC按键检测
    if (init_adc_button(adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "ADC按键初始化失败");
        return false;
    }

    mouse->initialized = true;
    ESP_LOGI(TAG, "体感鼠标初始化成功");
    return true;
}

/**
 * @brief 启动体感鼠标服务
 */
bool gesture_mouse_start(gesture_mouse_t *mouse) {
    if (mouse == NULL || !mouse->initialized) {
        ESP_LOGE(TAG, "体感鼠标未初始化");
        return false;
    }

    esp_err_t ret;

    // 初始化蓝牙控制器
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙控制器初始化失败: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙控制器启用失败: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid初始化失败: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid启用失败: %s", esp_err_to_name(ret));
        return false;
    }

    // 注册GAP和HID回调
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP回调注册失败: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_ble_hidd_profile_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HID配置文件初始化失败: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_ble_hidd_register_callbacks(hidd_event_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HID回调注册失败: %s", esp_err_to_name(ret));
        return false;
    }

    // 设置安全参数
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    // 创建任务
    xTaskCreate(&mouse_sensor_task, "mouse_task", 4096, mouse, 6, &mouse->mouse_task_handle);
    xTaskCreate(&hid_demo_task, "hid_task", 2048, mouse, 4, &mouse->hid_task_handle);
    
    ESP_LOGI(TAG, "体感鼠标服务启动完成");
    return true;
}

/**
 * @brief 停止体感鼠标服务
 */
void gesture_mouse_stop(gesture_mouse_t *mouse) {
    if (mouse == NULL) return;

    // 停止任务
    if (mouse->mouse_task_handle != NULL) {
        vTaskDelete(mouse->mouse_task_handle);
        mouse->mouse_task_handle = NULL;
    }
    
    if (mouse->hid_task_handle != NULL) {
        vTaskDelete(mouse->hid_task_handle);
        mouse->hid_task_handle = NULL;
    }

    // 停止蓝牙
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    ESP_LOGI(TAG, "体感鼠标服务已停止");
}

/**
 * @brief 释放体感鼠标资源
 */
void gesture_mouse_deinit(gesture_mouse_t *mouse) {
    if (mouse == NULL) return;

    gesture_mouse_stop(mouse);
    qmi8658_deinit(&mouse->imu_sensor);
    
    // 清理ADC资源引用（不删除ADC句柄，因为它是外部提供的）
    g_adc_handle = NULL;
    
    mouse->initialized = false;
    g_mouse = NULL;
    
    ESP_LOGI(TAG, "体感鼠标资源已释放");
}

// ==================== 内部函数实现 ====================

/**
 * @brief HID设备事件回调函数
 */
static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param) {
    switch(event) {
        case ESP_HIDD_EVENT_REG_FINISH:
            ESP_LOGI(TAG, "HID设备注册完成，状态: %d", param->init_finish.state);
            if (param->init_finish.state == ESP_GATT_OK) {
                ESP_LOGI(TAG, "设置设备名称: %s", HIDD_DEVICE_NAME);
                esp_ble_gap_set_device_name(HIDD_DEVICE_NAME);
                ESP_LOGI(TAG, "配置广播数据");
                esp_ble_gap_config_adv_data(&hidd_adv_data);
            } else {
                ESP_LOGE(TAG, "HID设备注册失败，状态: %d", param->init_finish.state);
            }
            break;
        case ESP_HIDD_EVENT_BLE_CONNECT:
            if (g_mouse) {
                g_mouse->hid_conn_id = param->connect.conn_id;
                g_mouse->ble_connected = true;
            }
            ESP_LOGI(TAG, "体感鼠标BLE连接已建立");
            break;
        case ESP_HIDD_EVENT_BLE_DISCONNECT:
            sec_conn = false;
            if (g_mouse) {
                g_mouse->ble_connected = false;
            }
            ESP_LOGI(TAG, "体感鼠标BLE连接已断开，重新开始广播");
            esp_ble_gap_start_advertising(&hidd_adv_params);
            break;
        default:
            break;
    }
}

/**
 * @brief GAP事件处理函数
 */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "BLE广播数据设置完成，开始广播");
        esp_ble_gap_start_advertising(&hidd_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "BLE广播启动成功");
        } else {
            ESP_LOGE(TAG, "BLE广播启动失败: %d", param->adv_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        sec_conn = param->ble_security.auth_cmpl.success;
        if (sec_conn) {
            ESP_LOGI(TAG, "体感鼠标BLE安全连接已建立");
        }
        break;
    default:
        break;
    }
}

/**
 * @brief 鼠标传感器任务
 */
static void mouse_sensor_task(void *pvParameters) {
    gesture_mouse_t *mouse = (gesture_mouse_t *)pvParameters;
    float gyro_x, gyro_y, gyro_z;
    int8_t mouse_x, mouse_y;
    uint8_t mouse_buttons = 0;
    button_state_t last_button_state = BUTTON_STATE_IDLE;
    TickType_t last_button_time = 0;
    
    while (1) {
        if (mouse->ble_connected && sec_conn) {
            // 读取陀螺仪数据
            if (qmi8658_read_gyro_dps(&mouse->imu_sensor, &gyro_x, &gyro_y, &gyro_z)) {
                float mouse_x_raw = gyro_y * MOUSE_SENSITIVITY;
                float mouse_y_raw = -gyro_x * MOUSE_SENSITIVITY;
                
                if (fabs(gyro_x) < GYRO_THRESHOLD) mouse_y_raw = 0;
                if (fabs(gyro_y) < GYRO_THRESHOLD) mouse_x_raw = 0;
                
                mouse_x = (int8_t)constrain(mouse_x_raw, -MOUSE_MAX_MOVE, MOUSE_MAX_MOVE);
                mouse_y = (int8_t)constrain(mouse_y_raw, -MOUSE_MAX_MOVE, MOUSE_MAX_MOVE);
            } else {
                mouse_x = 0;
                mouse_y = 0;
            }
            
            // 读取按键状态（带防抖处理）
            button_state_t current_button_state = read_button_state();
            TickType_t current_time = xTaskGetTickCount();
            
            if (current_button_state != last_button_state) {
                if ((current_time - last_button_time) > pdMS_TO_TICKS(BUTTON_DEBOUNCE_TIME)) {
                    last_button_state = current_button_state;
                    last_button_time = current_time;
                    
                    // 根据按键状态设置鼠标按键
                    switch (current_button_state) {
                        case BUTTON_STATE_LEFT:
                            mouse_buttons = 0x01;  // 左键
                            break;
                        case BUTTON_STATE_RIGHT:
                            mouse_buttons = 0x02;  // 右键
                            break;
                        case BUTTON_STATE_VOLUME_UP:
                            // 发送音量增加控制
                            esp_hidd_send_consumer_value(mouse->hid_conn_id, HID_CONSUMER_VOLUME_UP, true);
                            vTaskDelay(50 / portTICK_PERIOD_MS);
                            esp_hidd_send_consumer_value(mouse->hid_conn_id, HID_CONSUMER_VOLUME_UP, false);
                            mouse_buttons = 0;
                            break;
                        case BUTTON_STATE_VOLUME_DOWN:
                            // 发送音量减少控制
                            esp_hidd_send_consumer_value(mouse->hid_conn_id, HID_CONSUMER_VOLUME_DOWN, true);
                            vTaskDelay(50 / portTICK_PERIOD_MS);
                            esp_hidd_send_consumer_value(mouse->hid_conn_id, HID_CONSUMER_VOLUME_DOWN, false);
                            mouse_buttons = 0;
                            break;
                        default:
                            mouse_buttons = 0;
                            break;
                    }
                }
            } else if (current_button_state == BUTTON_STATE_IDLE) {
                mouse_buttons = 0;
            }
            
            // 发送鼠标数据
            if (mouse_x != 0 || mouse_y != 0 || mouse_buttons != 0) {
                esp_hidd_send_mouse_value(mouse->hid_conn_id, mouse_buttons, mouse_x, mouse_y);
                
                if (mouse_buttons != 0) {
                    vTaskDelay(50 / portTICK_PERIOD_MS);
                    esp_hidd_send_mouse_value(mouse->hid_conn_id, 0, 0, 0);
                }
            }
        }
        
        vTaskDelay(MOUSE_SAMPLE_RATE / portTICK_PERIOD_MS);
    }
}

/**
 * @brief HID演示任务
 */
static void hid_demo_task(void *pvParameters) {
    gesture_mouse_t *mouse = (gesture_mouse_t *)pvParameters;
    
    while(1) {
        // 简单的状态监控
        static bool last_ready_state = false;
        bool current_ready = (mouse->ble_connected && sec_conn);
        
        if (current_ready != last_ready_state) {
            if (current_ready) {
                ESP_LOGI(TAG, "体感鼠标已就绪");
            } else {
                ESP_LOGI(TAG, "体感鼠标未就绪");
            }
            last_ready_state = current_ready;
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief 约束数值范围
 */
static float constrain(float value, float min_val, float max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

/**
 * @brief 初始化ADC按键检测
 */
static esp_err_t init_adc_button(adc_oneshot_unit_handle_t adc_handle) {
    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "ADC handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    g_adc_handle = adc_handle;

    adc_oneshot_chan_cfg_t config = {
        .atten = GESTURE_BUTTON_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_oneshot_config_channel(g_adc_handle, GESTURE_BUTTON_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

/**
 * @brief 读取按键状态
 */
static button_state_t read_button_state(void) {
    if (g_adc_handle == NULL) {
        return BUTTON_STATE_IDLE;
    }
    
    int sum = 0;
    int valid_samples = 0;
    
    // 多次采样取平均值
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int adc_raw = 0;
        esp_err_t ret = adc_oneshot_read(g_adc_handle, GESTURE_BUTTON_ADC_CHANNEL, &adc_raw);
        if (ret == ESP_OK) {
            sum += adc_raw;
            valid_samples++;
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    
    if (valid_samples == 0) {
        return BUTTON_STATE_IDLE;
    }
    
    int avg_value = sum / valid_samples;
    
    // 根据ADC值判断按键状态
    if (avg_value >= BUTTON_IDLE_MIN) {
        return BUTTON_STATE_IDLE;
    } else if (avg_value <= BUTTON_UP_MAX) {
        return BUTTON_STATE_VOLUME_UP;
    } else if (avg_value >= BUTTON_RIGHT_MIN && avg_value <= BUTTON_RIGHT_MAX) {
        return BUTTON_STATE_RIGHT;
    } else if (avg_value >= BUTTON_LEFT_MIN && avg_value <= BUTTON_LEFT_MAX) {
        return BUTTON_STATE_LEFT;
    } else if (avg_value >= BUTTON_DOWN_MIN && avg_value <= BUTTON_DOWN_MAX) {
        return BUTTON_STATE_VOLUME_DOWN;
    }
    
    return BUTTON_STATE_IDLE;
}
