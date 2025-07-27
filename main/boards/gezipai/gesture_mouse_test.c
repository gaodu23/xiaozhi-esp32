/**
 * @file gesture_mouse_test.c
 * @brief 体感鼠标功能测试文件
 * 
 * 这个文件包含用于测试体感鼠标功能的测试函数
 */

#include "gesture_mouse.h"
#include "qmi8658.h"
#include "esp_log.h"

#define TAG "GESTURE_MOUSE_TEST"

/**
 * @brief 测试QMI8658传感器基本功能
 */
void test_qmi8658_sensor(void) {
    ESP_LOGI(TAG, "Testing QMI8658 sensor functionality...");
    
    // 这里可以添加传感器测试代码
    ESP_LOGI(TAG, "QMI8658 sensor test completed");
}

/**
 * @brief 测试体感鼠标初始化
 */
void test_gesture_mouse_init(void) {
    ESP_LOGI(TAG, "Testing gesture mouse initialization...");
    
    // 这里可以添加初始化测试代码
    ESP_LOGI(TAG, "Gesture mouse initialization test completed");
}

/**
 * @brief 运行所有体感鼠标测试
 */
void run_gesture_mouse_tests(void) {
    ESP_LOGI(TAG, "Starting gesture mouse tests...");
    
    test_qmi8658_sensor();
    test_gesture_mouse_init();
    
    ESP_LOGI(TAG, "All gesture mouse tests completed");
}
