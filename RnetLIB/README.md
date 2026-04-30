# RnetLIB — R-Net 轮椅 CAN 总线控制库

基于 ESP32-S3 TWAI + TJA1055 LSFT CAN 的 R-Net 轮椅控制器库。

## 功能概览

### 发送（控制）功能

| 功能 | 说明 |
|------|------|
| 摇杆驱动 | 10ms (100Hz) 周期发送 CAN 帧，模拟摇杆控制轮椅运动 |
| 座椅电推杆 | 50ms (20Hz) 周期发送，支持 4 路电机 |
| 硬件限位保护 | 自动监听限位反馈帧，超过限位时拒绝对应方向的运动 |
| 灯光控制 | 左转/右转/双闪/照明灯 (toggle 切换) |
| 喇叭/蜂鸣 | 喇叭开关 + 持续鸣叫 |
| 模式切换 | Drive ↔ Seating 模式切换 |
| 速度/配置 | 速度档位 (1-5) 和配置文件 (1-3) 切换 |
| 导航控制 | 自主导航启动/暂停/恢复/取消 |
| 攻击帧 | 发送 5 帧空帧获取总线控制权 |
| 转向修正 | 注册回调实现 IMU 闭环航向校正 |
| Omni 心跳 | (可选) 发送 Omni 特殊接口盒心跳帧，兼容特定底盘 |

### 接收（状态监控）功能

| 功能 | 帧 ID | 说明 |
|------|-------|------|
| 限位状态 | `0x0C140200` | 各推杆通道伸展/缩回方向的允许状态 |
| 座椅菜单 | `0x0C180201` | ISM 广播当前选中的座椅模式菜单项 |
| 驱动电机电流 | `0x14300000` | 16-bit LE 原始值，200ms 周期 |
| 电池电量 | `0x1C0C0000` | Byte0 = 0~100%，1000ms 周期 |
| 里程计 | `0x1C300004` | 左/右轮 32-bit LE 累计计数，1000ms 周期 |

## 硬件要求

- **MCU**: ESP32-S3 (支持 TWAI 外设)
- **CAN 收发器**: TJA1055 (LSFT CAN, ISO 11898-3)
- **协议**: R-Net CAN 2.0B 扩展帧, 125 kbps

### 硬件连接

```
ESP32-S3             TJA1055
────────             ───────
PIN_CAN_TX  ───────> TXD
PIN_CAN_RX  <─────── RXD
```

## 在 PlatformIO 项目中使用

### 1. 添加库依赖

```ini
[env:your_board]
platform = espressif32@6.12.0
board = esp32s3
framework = arduino

lib_deps =
    RnetLIB=../RnetLIB
```

### 2. 配置引脚 (build_flags)

```ini
build_flags =
    -DPIN_CAN_TX=GPIO_NUM_43
    -DPIN_CAN_RX=GPIO_NUM_44
```

### 3. 功能开关

```ini
build_flags =
    -DRNET_ENABLE_OMNI_HEARTBEAT=1   ; 启用 Omni 特殊接口盒心跳 (默认关闭)
```

### 4. 基本代码示例

```cpp
#include <Arduino.h>
#include "RNetController.h"

RNetController rnet;

void setup() {
    Serial.begin(115200);
    rnet.begin();
    if (rnet.wakeUp()) {
        Serial.println("R-Net 启动成功");
    }
}

void loop() {
    rnet.setJoystick(50, 0);   // 前进 50%
    delay(2000);
    rnet.setJoystick(0, 0);    // 停止
    delay(1000);
}
```

### 5. 座椅电推杆控制

```cpp
// 开始移动靠背电机（正方向 = 后仰）
rnet.moveActuator(RNET_MOTOR_RECLINE, true);
delay(3000);
rnet.stopActuator();
```

电机索引定义：

| 索引 | 宏定义 | 实际功能 | 正方向 (0x80) | 负方向 (0x40) |
|------|--------|----------|-------------|-------------|
| 0 | `RNET_MOTOR_TILT` | 俯仰 | 后倾 | 前倾 |
| 1 | `RNET_MOTOR_RECLINE` | 靠背 | 后仰 | 前倾 |
| 2 | `RNET_MOTOR_LEGS` | 腿托 | 降低 | 升高 |
| 3 | `RNET_MOTOR_3` | 预留 | — | — |

### 6. 灯光控制

R-Net 灯光帧为 **toggle** 命令，发一帧即切换对应灯的开/关状态。

```cpp
rnet.lampLeftOn();        // 左转向灯 toggle
rnet.lampRightOn();       // 右转向灯 toggle
rnet.lampHazardOn();      // 双闪 toggle
rnet.lampFloodOn();       // 照明灯开
rnet.lampAllOff();        // 关闭所有灯光
```

### 7. 喇叭和蜂鸣

```cpp
rnet.hornOn();             // 鸣笛
rnet.hornOff();            // 停止
rnet.hornBeep(500);        // 鸣笛 500ms
rnet.buzz();               // 蜂鸣提示音
```

### 8. 模式切换 / 速度档位

```cpp
rnet.seatMenuEnter();     // Drive → Seating 模式
rnet.seatMenuExit();      // Seating → Drive 模式
rnet.setSpeed(3);         // 速度档位 1-5
rnet.setProfile(2);       // 配置文件 1-3
```

### 9. 转向修正回调 (IMU 闭环)

```cpp
int8_t headingCorrection(int8_t speed, int8_t turn, float dt) {
    float yawError = getYawError();
    return turn + (int8_t)(yawError * kp);
}

rnet.setTurnCorrectionCallback(headingCorrection);
```

### 10. 导航控制

```cpp
rnet.navStart(0);         // 开始导航到目标点 0
rnet.navPause();          // 暂停
rnet.navResume(0);        // 恢复
rnet.navCancel();         // 取消
```

### 11. 读取底盘状态

以下数据由后台 `canRxTask` 自动解析：

```cpp
uint8_t pct = rnet.getBatteryPct();
// 0~100 = 百分比; 0xFF = 尚未收到

int16_t raw = rnet.getMotorCurrentRaw();
// 16-bit LE 原始值 (0x02E8 ≈ 6A，需实测标定)

uint32_t left  = rnet.getOdoLeft();
uint32_t right = rnet.getOdoRight();
// 累计脉冲，断电归零
```

#### 限位状态

```cpp
bool canExtend  = rnet.canMotorExtend(i);   // 允许正向（伸出）
bool canRetract = rnet.canMotorRetract(i);  // 允许反向（收回）
```

### 12. 统计计数器

```cpp
rnet.getTxCount();          // 摇杆帧发送成功次数
rnet.getTxErrorCount();     // 摇杆帧发送失败次数
rnet.getActTxCount();       // 电推杆帧发送次数
rnet.getActTxErrorCount();  // 电推杆帧发送失败次数
#if RNET_ENABLE_OMNI_HEARTBEAT
rnet.getOmniTxCount();
rnet.getOmniTxErrorCount();
#endif
```

## 主要 CAN 帧 ID

### 发送帧

| CAN ID | 用途 | DLC | 频率 |
|--------|------|-----|------|
| `0x02000400` | 摇杆心跳 | 2 | 100Hz (10ms) |
| `0x08080300` | 座椅电推杆 | 1 | 20Hz (50ms) |
| `0x0C000000` | 攻击帧 | 0 | 突发 5 帧 |
| `0x0C040400` | 喇叭开始 | 0 | 单次 |
| `0x0C040401` | 喇叭停止 | 0 | 单次 |
| `0x0C000401~404` | 灯光 toggle | 0 | 单次 |
| `0x181C0300` | 蜂鸣器 | 8 | 单次 |
| `0x03C30F0F` | Omni 心跳 (可选) | 8 | 10Hz (100ms) |

### 接收帧

| CAN ID | 用途 | DLC | 频率 |
|--------|------|-----|------|
| `0x0C140200` | 限位状态广播 | 1 | — |
| `0x0C180201` | 座椅菜单选项 | 1 | — |
| `0x14300000` | 驱动电机电流 | 2 | ~5Hz (200ms) |
| `0x1C0C0000` | 电池电量百分比 | 1 | 1Hz (1000ms) |
| `0x1C300004` | 里程计左/右计数 | 8 | 1Hz (1000ms) |

## 文件结构

```
RnetLIB/
├── README.md            本文档
├── RNetController.h     控制器类头文件 (公共 API + 常量定义)
└── RNetController.cpp   控制器类实现
```

## 状态机

```
begin()          wakeUp()          shutdown()
  │                 │                  │
  v                 v                  v
IDLE ──────────> RUNNING ──────────> IDLE
                    │
               ERROR │ (TWAI 启动失败)
```

## 注意事项

- 摇杆帧**必须**以 10ms 间隔持续发送，中断超过约 100ms 轮椅会触发紧急制动
- `wakeUp()` 会自动发送 5 帧攻击帧 (`0x0C000000`) 获取总线控制权
- 所有公共方法均为线程安全（FreeRTOS 互斥锁保护共享数据）
- 里程计和电流数据在 PM 断电后归零
- 电机电流帧 ID 的低字节取决于实际网络设备号（通常为 0 或 4），需抓包确认

