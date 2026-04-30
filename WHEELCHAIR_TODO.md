# 智能语音电动轮椅控制系统 — 开发 TODO

> 基于 **小智 ESP32** + **RnetLIB** + **Waveshare ESP32-S3-Touch-LCD-3.5B**

---

## 项目目标

将小智语音 AI 与 R-Net 电动轮椅 CAN 控制系统整合，实现：

1. **语音控制**（4G 云端 AI）：模式切换、座椅功能、语音播报状态
2. **TCP 远程控制**（本地 WiFi AP）：手机/PC 客户端发送方向指令实时控制行驶
3. **姿态感知**：实时获取轮椅俯仰角、横滚角、偏航角
4. **安全保护**：断线即停、看门狗、认证、急停

---

## 硬件平台

| 硬件 | 型号/规格 | 用途 |
|---|---|---|
| 主控 | Waveshare ESP32-S3-Touch-LCD-3.5B（ESP32-S3R8，8MB PSRAM，16MB Flash） | 全部功能集成 |
| CAN 收发器 | TJA1055（LSFT CAN，ISO 11898-3） | R-Net CAN 总线接口 |
| 4G 模块 | ML307 或兼容模组 | 小智语音云端 AI 连接 |
| 外置 IMU | 维特智能 HWT101（UART TTL） | Z 轴偏航角（轮椅转向测量） |
| 板载 IMU | QMI8658（I2C，已集成） | 俯仰角、横滚角（座椅倾斜状态） |
| CAN 总线 | R-Net 125kbps 2.0B 扩展帧 | 轮椅电机/电推杆/灯光控制 |

---

## 引脚分配（最终方案）

> 以下引脚在弃用摄像头接口后全部从摄像头信号释放，无冲突

| 功能 | ESP32-S3 引脚 | 说明 |
|---|---|---|
| TWAI/CAN TX | GPIO45 | 原 CAM_PIN_D0，接 TJA1055 TXD |
| TWAI/CAN RX | GPIO46 | 原 CAM_PIN_D3，接 TJA1055 RXD |
| HWT101 UART RX（ESP侧） | GPIO17 | 原 CAM_PIN_VSYNC，接 HWT101 TX |
| HWT101 UART TX（ESP侧） | GPIO18 | 原 CAM_PIN_HREF，接 HWT101 RX |
| 4G 模块 UART TX（ESP侧） | GPIO39 | 原 CAM_PIN_D6 |
| 4G 模块 UART RX（ESP侧） | GPIO40 | 原 CAM_PIN_D5 |
| QMI8658 I2C SDA | GPIO8 | 已集成，板载 I2C 总线 |
| QMI8658 I2C SCL | GPIO7 | 已集成，板载 I2C 总线 |
| TJA1055 EN/STB | —— | **本项目不使用睡眠模式，相关代码已删除** |

**注意（TX/RX 交叉连接）：**

- HWT101 模块 TX → ESP GPIO17（ESP UART RX）
- HWT101 模块 RX → ESP GPIO18（ESP UART TX）
- 4G 模块 TX → ESP GPIO40（ESP UART RX）
- 4G 模块 RX → ESP GPIO39（ESP UART TX）

---

## FreeRTOS 任务规划

| 任务名 | Core | 优先级 | 功能 |
|---|---|---|---|
| `rnet_heartbeat`（RnetLIB内部） | Core 1 | MAX-1（最高） | 10ms CAN 心跳，已固定，不可变 |
| `rnet_can_rx`（RnetLIB内部） | Core 1 | MAX-2 | CAN 帧接收/限位处理，已固定 |
| WiFi 协议栈（ESP-IDF内置） | Core 0 | ESP-IDF 固定 | WiFi AP + LwIP，系统强制 Core 0 |
| `xiaozhi_main`（小智主循环） | Core 0 | 中 | 语音 AI 事件处理 |
| `tcp_server_task`（新增） | Core 0 | 中 | TCP 监听/认证/指令接收 |
| `cmd_dispatcher_task`（新增） | Core 0 | 中-高 | 看门狗、互斥锁、调用 RnetLIB API |
| `imu_hwt101_task`（新增） | Core 0 | 低 | 解析 HWT101 UART 输出，更新偏航角 |
| `imu_qmi8658_task`（新增） | Core 0 | 低 | 读取 QMI8658，Mahony 滤波，更新俯仰/横滚 |
| LVGL 屏幕刷新（已有） | Core 0 | 低 | 状态显示 |

**原则：Core 1 只跑 RnetLIB，其余全部在 Core 0。**

---

## 分阶段开发任务

### 阶段 0：基础环境准备

- [x] 确认以 `main/boards/waveshare/esp32-s3-touch-lcd-3.5b` 为目标板编译小智
- [ ] 确认小智在该板上跑通基础功能：音频录放、WiFi 连接、4G 语音通话
- [x] 将 RnetLIB 添加到 ESP-IDF 的 CMakeLists 组件中（非 Arduino 框架迁移）
- [ ] 拿逻辑分析仪确认 TJA1055 接线正确，能在总线上看到 10ms 心跳帧

---

### 阶段 1：RnetLIB 清理和适配

**目标：去掉不用的唤醒电路代码，适配新引脚**

- [x] 删除 `RNetController.h` / `RNetController.cpp` 中 `PIN_WAKE_MOS_H`、`PIN_WAKE_MOS_L`、`PIN_TJA_EN`、`PIN_TJA_STB` 相关宏定义
- [x] 删除对应的 `gpio_set_direction`、`ledc_*` 初始化代码（TJA1055 EN/STB 脚和 LEDC PWM）
- [x] 删除 `RNetState::SLEEPING`、`RNetState::WAKING` 相关逻辑（直接进 RUNNING）
- [x] 通过 build_flags 设置 `PIN_CAN_TX=GPIO_NUM_45`、`PIN_CAN_RX=GPIO_NUM_46`
- [ ] 验证：上电后 CAN 攻击帧发出，RUNNING 状态正常，10ms 心跳稳定

---

### 阶段 2：TCP Server 和指令通道

**目标：最小控制闭环——客户端发命令，轮椅动**

- [x] 实现 `tcp_server_task`：监听固定端口，接受 TCP 连接，逐行解析命令
- [x] 定义指令协议（建议文本协议，每条以 `\n` 结尾）：

  | 指令 | 说明 |
  |---|---|
  | `FORWARD\n` | 前进（speed=50%） |
  | `BACKWARD\n` | 后退 |
  | `LEFT\n` | 左转 |
  | `RIGHT\n` | 右转 |
  | `STOP\n` | 停止（joystick 归零） |
  | `SPEED:75\n` | 设置速度百分比 |
  | `TILT_UP\n` / `TILT_DOWN\n` | 座椅前后倾 |
  | `RECLINE_UP\n` / `RECLINE_DOWN\n` | 靠背后仰/前倾 |
  | `LEGS_UP\n` / `LEGS_DOWN\n` | 腿托升降 |
  | `ACT_STOP\n` | 停止所有电推杆 |
  | `MODE_DRIVE\n` / `MODE_SEAT\n` | 驾驶/座椅模式切换 |

- [ ] 实现 FreeRTOS 队列（深度 8）连接 TCP 任务和 cmd_dispatcher 任务（当前直接调用，后期可拆分）
- [x] 实现 `cmd_dispatcher_task`：从队列读取指令，调用 RnetLIB API（当前集成在 tcp_client_task 中）

---

### 阶段 3：安全与失效保护

**目标：任何故障情况下轮椅不失控**

- [x] **TCP 断线即停**：客户端断开时立即调用 `rnet.setJoystick(0, 0)` + `rnet.stopActuator()`，进入 `SAFETY_SAFE_STOP`
- [x] **看门狗机制**：cmd_dispatcher 维护"最后指令时间戳"，超过 300ms 无新指令自动停止（与 `RNET_SEAT_CMD_TIMEOUT_MS=300ms` 保持一致）
- [x] **控制权互斥**：同时只允许一个 TCP 客户端持有控制权（记录 socket fd），第二个连入时踢掉旧连接
- [x] **TCP 认证**：连接建立后，客户端发送固定 Token（`AUTH wheelchair`），认证失败直接关闭 socket
- [x] **全局安全状态机**：实现 `SafetyState`（NORMAL / SAFE_STOP / ERROR / EMERGENCY），所有运动指令执行前先检查
- [x] **PWR 按键软急停**：boot_button_ 长按触发急停，调用 `WheelchairSetSafetyState(SAFETY_EMERGENCY)` + `stop()` + `seatStop()`，进入 EMERGENCY 状态

---

### 阶段 4：WiFi AP 模式配置

- [x] 将小智网络配置为 **AP 模式**：`CustomBoard` 改承承 `Ml307Board`， WiFi 唯独违行 AP 模式
- [x] 设置 AP 最大连接数 = 4
- [x] TCP Server 绑定 AP IP（`192.168.4.1`），监听端口 8899，随 AP 启动自动开启
- [x] **WiFi 连接后自动启动 TCP Server**：`SetNetworkEventCallback` 包装回调，`NetworkEvent::Connected` 时调用 `StartWheelchairTcpServer()`
- [x] 4G 连接使用 ML307 组件，仅服务语音 AI 通话链路
- [ ] 验证：4G 断线时本地 WiFi TCP 控制仍正常工作

---

### 阶段 5：HWT101 偏航角接入

**目标：读取轮椅 Z 轴转向量，供 IMU 闭环使用**

- [x] 配置 UART（GPIO17 RX、GPIO18 TX，115200bps，UART1）
- [x] 实现 `imu_hwt101_task`：循环读取 HWT101 输出帧，解析标准维特协议（0x55 帧头）
- [x] 提取 `Angle[2]`（Z 轴偏航角）字段，单位度（°）
- [x] 将偏航角数据暴露为全局原子变量 `s_yaw_deg`，供 IMU 显示和 MCP 工具读取
- [ ] 可选：通过 HWT101 UART 配置输出速率（建议 50Hz 或 100Hz，与 CAN 心跳同步）

---

### 阶段 6：QMI8658 俯仰角/横滚角接入

**目标：实时获取座椅倾斜姿态，用于安全限制和显示**

- [x] 确认 QMI8658 已在 I2C 总线（GPIO7/8）上可通信（地址 0x6B）
- [x] 实现 `imu_qmi8658_task`：读取加速度计 + 陀螺仪原始数据
- [x] 实现互补滤波（α=0.96），输出俯仰角（Pitch）和横滚角（Roll）
- [x] 将姿态数据暴露为全局原子变量 `s_pitch` / `s_roll`，供显示和 MCP 工具读取
- [ ] 可选：座椅超过安全角度阈值时，禁止继续同方向操作

---

### 阶段 7：语音 MCP 工具注册

**目标：语音命令可以控制轮椅模式和功能**

- [x] 注册 MCP 工具 `wheelchair_drive`：参数 `direction`（forward/backward/left/right/stop）、`speed_percent`（0-100），内部调用 cmd_dispatcher 同一逻辑
- [x] 注册 MCP 工具 `wheelchair_actuator`：参数 `motor`（tilt/recline/legs）、`direction`（up/down/stop）
- [x] 注册 MCP 工具 `wheelchair_mode`：参数 `mode`（drive/seat），调用 `rnet.setDriveMode()`（接口预留，RnetLIB 后续实现）
- [x] 注册 MCP 工具 `wheelchair_status`：返回当前速度、安全状态、电池电量（偏航角/俯仰角在 Phase5/6 后补充）
- [x] MCP 工具调用期间设置 `s_mcp_controlling=true`，阻塞 TCP 运动指令（语音优先）
- [x] 语音和 TCP 控制优先级：语音命令优先；`wheelchair_drive` 执行期间 TCP 运动指令返回 `ERR BLOCKED`

---

### 阶段 8：屏幕状态显示

**目标：3.5 寸屏实时显示关键状态**

- [ ] 显示区域划分建议：

  | 区域 | 内容 |
  |---|---|
  | 顶栏 | 网络状态（WiFi AP / 4G信号格数） |
  | 左侧 | 控制模式（Drive/Seat）、TCP 客户端状态（已连/未连/认证中） |
  | 中央 | 当前速度档位、摇杆方向指示（箭头） |
  | 右侧 | 电池电量（从 RnetLIB 获取 `RNET_BATTERY_CAN_ID`） |
  | 底栏 | 偏航角（HWT101）、俯仰角/横滚角（QMI8658）、安全状态（NORMAL/STOP/ERROR） |

- [x] **底栏 IMU 覆盖层**：`CustomLcdDisplay::SetupUI()` 创建 LVGL 标签，格式 `Y:+45.2° P:+3.1° R:-1.5° [SAFE]`，5Hz 定时刷新
- [ ] 安全状态异常时顶部显示红色告警横幅
- [ ] RNetState 变为 ERROR 时全屏告警提示

---

### 阶段 9：压测与联调

| 测试项 | 通过标准 |
|---|---|
| CAN 心跳稳定性压测（同时满载 WiFi+音频+TCP） | 逻辑分析仪测 10ms 心跳，抖动 < ±1ms |
| TCP 断线触发停止 | 客户端断开后 300ms 内 setJoystick(0,0) 执行 |
| 看门狗超时停止 | 保持连接但停发指令，300ms 后自动停止 |
| 第二客户端踢旧逻辑 | 第二个连入后旧客户端立即断开 |
| 认证失败拒绝 | 错误 token 的连接立即关闭，不执行任何指令 |
| 语音模式切换期间 TCP 被阻塞 | 切换完成前 TCP 方向指令不执行 |
| 硬件限位触发后拒绝同方向指令 | RnetLIB 拒绝发送，日志有记录 |
| 4G 断线语音降级本地可控 | 4G 无信号时 TCP 控制不受影响，语音提示降级 |
| 内存稳定性（连续运行 4 小时） | 堆最小值稳定，无 OOM |

---

## 开发优先级

```
P0（必须先做）
  └── 阶段0：基础环境确认（编译目标板+小智跑通）
  └── 阶段1：RnetLIB 清理适配（CAN 心跳稳定是一切基础）

P1（最小可用版本）
  └── 阶段2：TCP Server + 指令通道（控制闭环）
  └── 阶段3：安全保护（做真实测试前必须有）
  └── 阶段4：WiFi AP 配置

P2（功能完善）
  └── 阶段5：HWT101 偏航角
  └── 阶段6：QMI8658 俯仰角/横滚角
  └── 阶段7：语音 MCP 工具

P3（体验提升）
  └── 阶段8：屏幕状态显示
  └── 阶段9：完整压测
```

---

## 注意事项

1. **RnetLIB 非 Arduino**：原库基于 Arduino 框架，集成到小智 ESP-IDF 工程需要做框架兼容处理（Arduino-as-component 或重写为纯 ESP-IDF 驱动）
2. **CAN 总线控制权**：R-Net 需要发送 5 帧空帧"攻击"获取控制权，每次上电后需执行一次
3. **TJA1055 供电**：TJA1055 工作在 LSFT CAN（低速容错 CAN），正常模式 EN=1 STB=1，本项目去掉了软件控制，接线时确保这两脚硬件上拉到高电平
4. **HWT101 默认波特率**：产品规格书中有一则评论提示默认波特率可能有出入，首次连接时用串口调试工具确认实际波特率
5. **GPIO45/46 是 ESP32-S3 Strapping 引脚**：上电瞬间有短暂高/低电平影响，TJA1055 在上电期间可能收到噪声帧，初始化代码中注意等待 TJA1055 稳定后再使能 TWAI
