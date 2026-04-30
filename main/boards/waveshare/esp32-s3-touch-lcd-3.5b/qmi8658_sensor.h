#pragma once

#include <driver/i2c_master.h>

/**
 * QMI8658 6-DOF IMU 驱动
 * 接口：I2C (GPIO7 SDA / GPIO8 SCL)，地址 0x6B
 * 输出：俯仰角 (Pitch) 和横滚角 (Roll)，单位：度
 */

/**
 * 初始化 QMI8658 并启动数据采集任务。
 * @param i2c_bus 已初始化的 I2C 主机总线句柄
 * 应在 I2C 总线初始化之后调用，调用一次即可。
 */
void QMI8658Init(i2c_master_bus_handle_t i2c_bus);

/**
 * 获取最新俯仰角和横滚角。
 * @param pitch  输出俯仰角，单位度 (-90 ~ +90)，未初始化时为 0
 * @param roll   输出横滚角，单位度 (-180 ~ +180)，未初始化时为 0
 */
void QMI8658GetAttitude(float* pitch, float* roll);
