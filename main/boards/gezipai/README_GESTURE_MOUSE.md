# 体感鼠标集成说明

## 概述
本项目已成功将体感鼠标功能集成到小智ESP32项目中。体感鼠标使用QMI8658 6轴IMU传感器，通过BLE HID协议实现无线鼠标功能。

## 硬件要求
- ESP32S3芯片
- QMI8658 6DOF IMU传感器
- 与音频编解码器共用I2C总线

## 软件架构

### 核心文件
- `qmi8658.h/.c` - QMI8658传感器驱动
- `gesture_mouse.h/.c` - 体感鼠标主要实现
- `hid_dev.h` - HID设备定义
- `hidd_le_prf_int.h` - HID配置文件内部定义
- `hid_device_le_prf.c` - HID设备配置文件实现
- `esp_hidd_prf_api.h/.c` - ESP HID设备API

### 集成点
- 在`gezipai_board.cc`中集成体感鼠标
- 使用专用I2C总线（I2C_NUM_1）用于QMI8658传感器
- 使用ADC检测五向按键状态
- 在板子初始化时自动启动体感鼠标功能

## 配置文件更新

### sdkconfig 更新
添加了以下蓝牙配置：
- `CONFIG_BT_ENABLED=y` - 启用蓝牙
- `CONFIG_BT_BLUEDROID_ENABLED=y` - 启用Bluedroid蓝牙栈
- `CONFIG_BT_BLE_ENABLED=y` - 启用BLE功能
- `CONFIG_BT_GATTS_ENABLE=y` - 启用GATT服务器
- 各种BLE HID相关的配置选项

### GPIO配置
- QMI8658传感器使用专用I2C总线：
  - SDA: GPIO_NUM_11
  - SCL: GPIO_NUM_12
- 五向按键ADC检测：
  - ADC输入: GPIO_NUM_9 (ADC1_CH8)

## 功能特性
1. **自动启动** - 设备开机后自动启动体感鼠标功能
2. **独立运行** - 体感鼠标与小智其他功能独立运行，不需要MCP控制
3. **专用硬件** - 使用专用I2C总线和ADC按键检测
4. **BLE HID** - 标准BLE HID协议，兼容各种设备
5. **手势识别** - 基于陀螺仪数据的鼠标移动控制
6. **多功能按键** - 支持鼠标左右键、音量控制等功能

## 按键功能
- **左侧按键** - 鼠标左键点击
- **右侧按键** - 鼠标右键点击  
- **上方按键** - 系统音量增加
- **下方按键** - 系统音量减少
- **中间按键** - 无功能（预留扩展）

## 传感器配置
- 陀螺仪范围：±512 dps
- 加速度计范围：±2g
- 输出数据率：125Hz
- I2C地址：0x6A/0x6B

## 使用说明
1. 设备开机后自动启动体感鼠标功能
2. 设备会出现为BLE HID鼠标设备
3. 在目标设备上搜索并连接蓝牙鼠标
4. 移动设备即可控制鼠标光标移动

## 开发说明
- 所有体感鼠标相关代码位于`main/boards/gezipai/`目录
- 使用现代ESP-IDF I2C master API
- 遵循ESP-IDF编程规范
- 支持FreeRTOS任务并发

## 故障排除
1. **传感器初始化失败** - 检查I2C连接和地址配置
2. **BLE连接失败** - 确认蓝牙配置正确启用
3. **内存不足** - 项目已配置足够的堆栈大小，如遇问题可适当增加

## 版本信息
- 基于原始BLE HID演示项目
- 适配ESP-IDF 5.4.1
- 支持ESP32S3芯片
- 集成到小智项目v1版本
