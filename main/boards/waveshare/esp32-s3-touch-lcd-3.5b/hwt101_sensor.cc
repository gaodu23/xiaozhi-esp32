#include "hwt101_sensor.h"
#include "config.h"

#include <math.h>
#include <string.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <atomic>

#define TAG "HWT101"

/* 维特智能标准帧常量 */
#define WIT_HEADER      0x55u
#define WIT_ANGLE_TYPE  0x53u   // 角度帧: roll / pitch / yaw
#define WIT_FRAME_LEN   11      // header + type + 8 data bytes + checksum

static std::atomic<float> s_yaw_deg{0.0f};

/* ---- UART 接收任务 ---- */
static void hwt101_task(void* /*arg*/)
{
    int     idx = 0;
    uint8_t frame[WIT_FRAME_LEN];

    while (true) {
        uint8_t byte;
        int n = uart_read_bytes(HWT101_UART_NUM, &byte, 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;

        /* 简单状态机：帧头同步 */
        if (idx == 0) {
            if (byte != WIT_HEADER) continue;
        } else if (idx == 1) {
            if (byte != WIT_ANGLE_TYPE) {
                idx = 0;
                continue;   // 不是角度帧，重新同步
            }
        }

        frame[idx++] = byte;

        if (idx < WIT_FRAME_LEN) continue;  // 帧未接收完

        /* 帧接收完毕，验证校验和 */
        uint8_t sum = 0;
        for (int i = 0; i < WIT_FRAME_LEN - 1; i++) sum += frame[i];
        if (sum != frame[WIT_FRAME_LEN - 1]) {
            ESP_LOGW(TAG, "校验和错误 0x%02X != 0x%02X", sum, frame[WIT_FRAME_LEN - 1]);
            idx = 0;
            continue;
        }

        /* 解析偏航角 (字节 6-7，小端 int16) */
        int16_t raw_yaw = (int16_t)(((uint16_t)frame[7] << 8) | frame[6]);
        float   yaw_deg = raw_yaw / 32768.0f * 180.0f;
        s_yaw_deg.store(yaw_deg, std::memory_order_relaxed);

        idx = 0;
    }
}

void HWT101Init()
{
    uart_config_t cfg = {};
    cfg.baud_rate  = HWT101_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(HWT101_UART_NUM, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(HWT101_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(HWT101_UART_NUM,
                                  HWT101_TX_PIN,  /* TX：发给传感器 */
                                  HWT101_RX_PIN,  /* RX：接收传感器数据 */
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreatePinnedToCore(hwt101_task, "HWT101",
                             2048, nullptr, 3, nullptr, 0);
    ESP_LOGI(TAG, "HWT101 初始化完成，UART%d  RX=GPIO%d",
             (int)HWT101_UART_NUM, (int)HWT101_RX_PIN);
}

float HWT101GetYaw()
{
    return s_yaw_deg.load(std::memory_order_relaxed);
}