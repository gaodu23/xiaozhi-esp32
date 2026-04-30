#pragma once

/**
 * HWT101 单轴偏航角传感器驱动
 * 协议：维特智能标准帧 (0x55, DataType, 8字节数据, 校验和)
 * 引脚：HWT101_RX_PIN / HWT101_TX_PIN (来自 config.h)
 * 串口：HWT101_UART_NUM @ HWT101_BAUD
 */

/**
 * 初始化 HWT101 UART 并启动数据接收任务。
 * 应在 I2C / UART 硬件初始化之后调用，调用一次即可。
 */
void HWT101Init();

/**
 * 返回最新偏航角 (Z 轴)，单位：度 (-180 ~ +180)。
 * 未收到数据时返回 0.0f。
 */
float HWT101GetYaw();
