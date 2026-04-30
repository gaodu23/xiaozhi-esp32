/**
 * @file RNetController.h
 * @brief R-Net 轮椅控制器类——基于 ESP32-S3 TWAI + TJA1055 LSFT CAN
 *
 * 摇杆帧 (0x02000400): 10ms 100Hz 持续发送
 * 座椅帧 (0x08080300): 50ms 20Hz, 仅在操作时发送
 *
 * TJA1055 模式: EN=0,STB=0 Sleep | EN=1,STB=1 Normal
 */

#ifndef RNET_CONTROLLER_H
#define RNET_CONTROLLER_H

#include "driver/gpio.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

/** 转向修正回调类型——在 10ms 心跳任务中调用，用于 IMU 闭环航向校正等场景 */
typedef int8_t (*TurnCorrectionCallback)(int8_t speed, int8_t turn, float dt);

/* ======================== 默认引脚定义 ======================== */

#ifndef PIN_CAN_TX
#define PIN_CAN_TX GPIO_NUM_45
#endif

#ifndef PIN_CAN_RX
#define PIN_CAN_RX GPIO_NUM_46
#endif

/* ======================== R-Net 摇杆协议常量 ======================== */

/** R-Net 摇杆控制帧 CAN ID */
#define RNET_JOYSTICK_CAN_ID 0x02000400

/** 摇杆帧数据长度 */
#define RNET_JOYSTICK_DLC 2

/** 心跳发送周期 (ms) */
#define RNET_TX_PERIOD_MS 10

/** 摇杆值范围 [-127, +127] */
#define RNET_JOY_MIN (-127)
#define RNET_JOY_MAX (127)

/* ======================== R-Net 座椅/电推杆协议常量 ======================== */

/** 座椅控制帧 CAN ID */
#define RNET_ACTUATOR_CAN_ID 0x08080300

/** 座椅帧数据长度 */
#define RNET_ACTUATOR_DLC 1

/** 座椅帧 50ms 周期 (10ms * 5) */
#define RNET_ACTUATOR_TICK_DIVISOR 5

/** 停止后持续发 idle 帧的 tick 数 (500ms) */
#define RNET_ACTUATOR_STOP_FLUSH_TICKS 50

/** 灯光帧重发间隔 (500ms) */
#define RNET_LAMP_TICK_DIVISOR 50

/** 喜叭默认持续 tick 数 (500ms) */
#define RNET_HORN_DEFAULT_TICKS 50

/** 方向标志位 */
#define RNET_ACT_DIR_POSITIVE 0x80 // Bit 7: 正方向 (伸展)
#define RNET_ACT_DIR_NEGATIVE 0x40 // Bit 6: 负方向 (收缩) [推测]

/** 电机索引掩码 (低4位) */
#define RNET_ACT_MOTOR_MASK 0x0F

/** 最大电机索引 */
#define RNET_ACT_MOTOR_MAX 3

/* ======================== 电机索引快捷定义 ======================== */

#define RNET_MOTOR_TILT 0    // 倾斜
#define RNET_MOTOR_RECLINE 1 // 靠背
#define RNET_MOTOR_LEGS 2    // 腿托
#define RNET_MOTOR_3 3       // 预留


/* ======================== 硬件限位保护 ======================== */

/** 限位状态广播帧 CAN ID (29-bit 扩展帧) */
#define RNET_LIMIT_CAN_ID 0x0C140200UL

/**
 * @brief 限位帧 data[0] 状态位掩码：
 *   Bit 3 (0x08): 允许缩回 | Bit 2 (0x04): 允许伸出 | Bit 0 (0x01): 通道在线
 */
#define RNET_LIMIT_BIT_ONLINE  0x01U
#define RNET_LIMIT_BIT_EXTEND  0x04U
#define RNET_LIMIT_BIT_RETRACT 0x08U

/** 限位通道索引范围 (data[0] 低4位) */
#define RNET_LIMIT_CHANNEL_COUNT 16

/** 座椅菜单选项帧 (ISM→JSM): data[0]=0x2X 表示菜单第X项 */
#define RNET_SEAT_MENU_CAN_ID 0x0C180201UL

/* ======================== 速度/配置文件/座椅菜单 CAN ID ======================== */

/** 蜂鸣器控制帧 CAN ID (29-bit EFF) - DLC=8 */
#define RNET_BUZZ_CAN_ID       0x181C0300UL
/** 菜单控制 CAN ID (11-bit SFF) - DLC=4 (进入/退出菜单) */
#define RNET_MENU_CAN_ID       0x00000062UL

/** 灯光控制帧 CAN ID (29-bit EFF) - DLC=2, [mask, state] */
#define RNET_LAMP_CAN_ID       0x0C000400UL
/** 灯光位图: 左转=0x01, 右转=0x04, 双闪=0x10, 照明=0x80 */
#define RNET_LAMP_LEFT_TURN    0x01U
#define RNET_LAMP_RIGHT_TURN   0x04U
#define RNET_LAMP_HAZARD       0x10U
#define RNET_LAMP_FLOOD        0x80U
/** 单灯控制帧 CAN ID (29-bit EFF) - DLC=0, 按协议文档 0C000401-404 */
#define RNET_LAMP_LEFT_CAN_ID    0x0C000401UL
#define RNET_LAMP_RIGHT_CAN_ID   0x0C000402UL
#define RNET_LAMP_HAZARD_CAN_ID  0x0C000403UL
#define RNET_LAMP_FLOOD_CAN_ID   0x0C000404UL

/** 喇叭开始帧 CAN ID (29-bit EFF) - DLC=0 */
#define RNET_HORN_CAN_ID       0x0C040400UL
/** 喇叭停止帧 CAN ID (29-bit EFF) - DLC=0 (协议: 0C040401#) */
#define RNET_HORN_STOP_CAN_ID  0x0C040401UL

/** 速度档位切换 CAN ID (29-bit EFF) - DLC=1 */
#define RNET_SPEED_CAN_ID      0x0A040400UL
/** 配置文件切换 CAN ID (11-bit SFF) - DLC=4 */
#define RNET_PROFILE_CAN_ID    0x00000054UL
/** 座椅菜单控制 CAN ID (11-bit SFF) - DLC=4 (进入/退出座椅菜单帧序列) */
#define RNET_SEAT_MENU_CMD_CAN_ID  0x00000065UL
/** 显示运动速度信息 CAN ID (29-bit EFF) - DLC=2 */
#define RNET_SPEED_INFO_CAN_ID     0x0C180500UL

/** PMtx 电池电量 CAN ID (29-bit EFF) - DLC=1, Xx=0x00-0x64 (0-100%) */
#define RNET_BATTERY_CAN_ID        0x1C0C0000UL
/** PMtx 驱动电机电流 CAN ID (29-bit EFF) - DLC=2, LE uint16 (0x02E8≈6A) */
#define RNET_MOTOR_CURRENT_CAN_ID  0x14300000UL
/** PMtx 里程计 CAN ID (29-bit EFF) - DLC=8, LE uint32 左轮 + LE uint32 右轮 */
#define RNET_ODOMETER_CAN_ID       0x1C300004UL

/** 座椅持续命令超时 (ms) */
#define RNET_SEAT_CMD_TIMEOUT_MS   300
/** 导航持续命令超时 (ms) */
#define RNET_NAV_CMD_TIMEOUT_MS    300
/** 菜单导航脉冲时长 (ms) */
#define RNET_NAV_PULSE_MS          500
/** 菜单导航休息时长 (ms) */
#define RNET_NAV_REST_MS           1000
/** 菜单导航摇杆幅度 */
#define RNET_MENU_NAV_SPEED        80

/* ======================== FreeRTOS 任务配置 ======================== */
#define RNET_TASK_STACK_SIZE 4096
#define RNET_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define RNET_TASK_CORE 1

/* ======================== 类定义 ======================== */

/**
 * @brief R-Net 控制器状态枚举
 */
enum class RNetState : uint8_t
{
    IDLE = 0, // 未初始化
    RUNNING,  // 正常运行，心跳发送中
    ERROR     // 错误状态
};

/**
 * @brief 电推杆工作状态
 *
 * 状态转换:
 *   ACT_IDLE --[moveActuator]--> ACT_MOVING
 *   ACT_MOVING --[stopActuator]--> ACT_STOPPING  (发送 idle 帧刷新期)
 *   ACT_STOPPING --[flush 完成]--> ACT_IDLE
 *   任何状态 --[actuatorEmergencyStop]--> ACT_IDLE (立即)
 */
enum class ActuatorState : uint8_t
{
    ACT_IDLE = 0, // 空闲，不发送座椅帧
    ACT_MOVING,   // 运动中，周期性发送带方向标志的控制帧
    ACT_STOPPING  // 停止中，发送 idle 帧确保 ECU 收到
};

/**
 * @brief R-Net 轮椅控制器主类
 *
 * 管理 TJA1055 收发器模式切换、R-Net 总线唤醒、以及 10ms 周期的
 * 摇杆/心跳帧发送。使用 FreeRTOS 任务保证严格定时。
 */
class RNetController
{
public:
    RNetController();
    ~RNetController();

    /**
     * @brief 初始化控制器 (配置 GPIO，不启动 CAN)
     * @return true 初始化成功
     */
    bool begin();

    /**
     * @brief 设置摇杆值
     * @param speed  速度值 [-100(后退) ~ +100(前进)]
     * @param turn   转向值 [-100(左转) ~ +100(右转)]
     *
     * 值会被自动钳位到 [-100, +100] 范围。
     * 下一个 10ms 周期会自动发送此值。
     */
    void setJoystick(int8_t speed, int8_t turn);

    /**
     * @brief 紧急停止 — 立即发送 [0,0] 帧
     *
     * 将摇杆值强制归零，并立即发送一帧停止指令，
     * 不等待下一个 10ms 周期。
     */
    void stop();

    /**
     * @brief 关闭系统 — 停止心跳任务，卸载 CAN 驱动，TJA1055 进入 Sleep
     */
    void shutdown();

    /* ==================== 座椅/电推杆控制 ==================== */

    /**
     * @brief 开始移动指定电机
     *
     * 心跳任务会以 50ms 周期持续发送运动帧，
     * 直到调用 stopActuator() 或 actuatorEmergencyStop()。
     *
     * @param motorIndex 电机索引 (0=Tilt, 1=Recline, 2=Legs, 3=预留)
     * @param positive   true=正方向(0x80), false=负方向(0x40)
     */
    void moveActuator(uint8_t motorIndex, bool positive = true);

    /**
     * @brief 安全停止当前电机
     *
     * 进入 STOPPING 状态：持续发送 idle 帧 (不带方向标志)
     * 至少 500ms，确保轮椅 ECU 收到停止指令。
     * 刷新期结束后自动进入 IDLE，停止发送。
     */
    void stopActuator();

    /**
     * @brief 紧急停止电推杆 — 立即发送一帧 idle 并停止
     *
     * 跳过 flush 阶段，立即切换到 ACT_IDLE。
     * 用于异常情况。正常操作应使用 stopActuator()。
     */
    void actuatorEmergencyStop();

    /* ==================== 蜂鸣器 / 菜单 ==================== */

    /**
     * @brief 触发蜂鸣器 (0x181C0300 [02 60 ...])
     * @return true 发送成功
     */
    bool buzz();

    /* ==================== 灯光控制 ==================== */

    /** 开启左转灯 (0x0C000400 mask=0x01 state=0x01) */
    bool lampLeftOn();

    /** 开启右转灯 (0x0C000400 mask=0x04 state=0x04) */
    bool lampRightOn();

    /** 开启双闪 (0x0C000400 mask=0x15 state=0x10) */
    bool lampHazardOn();

    /** 开启照明灯 (0x0C000400 mask=0x80 state=0x80) */
    bool lampFloodOn();

    /** 关闭所有灯光 (0x0C000400 mask=0xFF state=0x00) */
    bool lampAllOff();

    /* ==================== 喇叭控制 ==================== */

    /** 喇叭持续鸣响 (DLC=0) */
    bool hornOn();

    /** 喇叭停止 (DLC=1, data[0]=0x01) */
    bool hornOff();

    /**
     * @brief 喇叭短鸣指定时长
     * @param duration_ms  持续毫秒数 (默认 500ms)
     * @return true 成功
     */
    bool hornBeep(uint32_t duration_ms = 500);

    /**
     * @brief 进入座椅菜单 (0x063 序列: [40 00 00 00] → [00 01 00 00])
     * @return true 两帧都发送成功
     */
    bool seatMenuEnter();

    /**
     * @brief 退出座椅菜单 (0x063 序列: [40 01 00 00] → [00 00 00 00])
     * @return true 两帧都发送成功
     */
    bool seatMenuExit();

    /* ==================== 配置命令 ==================== */

    /**
     * @brief 切换速度档位 (1~5)
     * @param level 档位 1(最慢)~5(最快)
     * @return true 发送成功
     */
    bool setSpeed(uint8_t level);

    /**
     * @brief 切换配置文件 (1~3)
     * @param profile 配置文件编号 1~3
     * @return true 发送成功
     */
    bool setProfile(uint8_t profile);

    /* ==================== 普通座椅控制模式 ==================== */

    /**
     * @brief 座椅电机运动 (普通模式)
     *
     * 激活座椅模式并驱动指定电机。需发送端持续调用,
     * 超时 300ms 未收到新指令则自动停止。
     *
     * @param motorIndex 电机索引 (0~3)
     * @param positive   true=正向, false=反向
     */
    void seatMove(uint8_t motorIndex, bool positive);

    /**
     * @brief 停止座椅运动 / 退出普通座椅模式
     */
    void seatStop();

    /* ==================== 特殊座椅控制模式 ==================== */

    /**
     * @brief 特殊座椅准备阶段
     *
     * 进入特殊座椅模式, 自动发送进入菜单帧序列,
     * 然后在 seatTick() 中自动导航到目标菜单项。
     *
     * @param targetItem 目标菜单项索引 (0~15)
     */
    void seatSpecialPrepare(int8_t targetItem);

    /**
     * @brief 特殊座椅执行阶段
     *
     * 将 speed 作为摇杆前/后值输出。需持续调用,
     * 超时 300ms 未收到则摇杆归零。
     *
     * @param speed 摇杆值 [-100, +100]
     */
    void seatSpecialExec(int8_t speed);

    /**
     * @brief 退出特殊座椅模式
     */
    void seatSpecialExit();

    /**
     * @brief 重置所有座椅控制状态并立即停止
     *
     * 包含: 停止电推杆 + 摇杆归零 + 清除所有座椅模式标志。
     * 调用时机: 紧急停止、发送端线等。
     */
    void seatModeReset();

    /**
     * @brief 座椅控制周期性调用 — 在 loop() 中调用
     *
     * 处理:
     *   - 普通座椅模式: SEAT_MOVE 超时自动停止
     *   - 特殊座椅模式: 菜单导航脉冲 + 执行阶段超时
     */
    void seatTick();

    /** 普通座椅模式是否激活 */
    bool isSeatModeActive() const { return _seatModeActive; }

    /** 特殊座椅模式是否激活 */
    bool isSpecialModeActive() const { return _specialModeActive; }

    /** 特殊座椅准备阶段是否完成 */
    bool isSpecialPrepDone() const { return _specialPrepDone; }

    /* ==================== 自主导航控制 ==================== */

    /**
     * @brief 开始导航 (首次启动)
     *
     * 进入导航模式并记录目标点。需发送端持续调用,
     * 超时 300ms 未收到则自动暂停。
     *
     * @param dest 目标点索引 (0=餐桌, 1=办公桌, 2=卧室)
     */
    void navStart(uint8_t dest);

    /**
     * @brief 暂停导航 — 摇杆归零，保留目标
     */
    void navPause();

    /**
     * @brief 恢复导航 (从暂停状态恢复)
     *
     * 必须在 navStart 之后调用。需发送端持续调用,
     * 超时 300ms 未收到则自动暂停。
     *
     * @param dest 目标点索引 (0=餐桌, 1=办公桌, 2=卧室)
     */
    void navResume(uint8_t dest);

    /**
     * @brief 取消导航 — 摇杆归零，退出导航模式
     */
    void navCancel();

    /**
     * @brief 重置导航状态
     */
    void navReset();

    /** 导航模式是否激活 */
    bool isNavActive() const { return _navModeActive; }

    /** 导航是否正在执行 (未暂停) */
    bool isNavRunning() const { return _navRunning; }

    /** 获取当前导航目标点 */
    uint8_t getNavDest() const { return _navDest; }

    /**
     * @brief 在 1ms 内连续发送 5 次"攻击"帧
     *
     * 帧格式:
     *   CAN ID:  0x0C000000 (29-bit 扩展帧)
     *   DLC:     0 (数据长度为 0，无数据字节)
     *   类型:    Data Frame（非 RTR）
     *
     * 5 帧将以尽可能短的间隔（约 192μs/帧 @125kbps）连续入队，
     * 整个突发过程在 1ms 内完成。
     * 要求控制器处于 RUNNING 状态（TWAI 驱动已启动）。
     *
     * @return 成功入队的帧数 (0~5)
     */
    int sendAttackFrameBurst();

    /* ==================== 硬件限位保护 ==================== */

    /**
     * @brief 配置电机索引与限位 CAN 通道号的映射
     *
     * 限位广播帧 data[0] 的低4位是 CAN 通道号，其数值未必等于电机索引 (0~3)。
     * 例如电机 0 对应通道 0x06，电机 1 对应 0x07，需通过此函数建立映射。
     * 默认映射: 电机 i → 通道 i (即不做偏移)。
     *
     * @param motorIndex  电机索引 (0~RNET_ACT_MOTOR_MAX)
     * @param canChannel  对应的限位帧通道号 (低4位, 0~RNET_LIMIT_CHANNEL_COUNT-1)
     */
    void setLimitChannel(uint8_t motorIndex, uint8_t canChannel);

    /**
     * @brief 查询指定电机当前是否允许向前/伸出
     *
     * 基于最近一次收到的 0x0C140200 限位帧缓存值。
     * 若尚未收到过对应通道的帧，返回 true（不阻止运动）。
     *
     * @param motorIndex 电机索引 (0~RNET_ACT_MOTOR_MAX)
     * @return true=允许伸出, false=顶部限位触发
     */
    bool canMotorExtend(uint8_t motorIndex) const;

    /**
     * @brief 查询指定电机当前是否允许向后/缩回
     *
     * @param motorIndex 电机索引 (0~RNET_ACT_MOTOR_MAX)
     * @return true=允许缩回, false=底部限位触发
     */
    bool canMotorRetract(uint8_t motorIndex) const;

    /**
     * @brief 获取当前座椅菜单选中项索引
     * @return  0~15 = 当前选中项编号, -1 = 不在座椅菜单中
     */
    int8_t getSeatMenuItem() const { return _seatMenuItem; }

    /* ==================== 状态查询 ==================== */

    /** 获取控制器主状态 */
    RNetState getState() const { return _state; }

    /** 获取摇杆 TX 计数 */
    uint32_t getTxCount() const { return _txCount; }

    /** 获取摇杆 TX 失败计数 */
    uint32_t getTxErrorCount() const { return _txErrorCount; }

    /** 获取当前速度设定值 */
    int8_t getSpeed() const { return _speed; }

    /** 获取当前转向设定值 */
    int8_t getTurn() const { return _turn; }

    /** 获取电池电量百分比 (0-100%; 0xFF=尚未收到) */
    uint8_t getBatteryPct() const { return _batteryPct; }

    /** 获取驱动电机电流原始值 (LE uint16; 0x02E8≈6A; 0=尚未收到) */
    uint16_t getMotorCurrentRaw() const { return _motorCurrentRaw; }

    /** 获取左轮里程计原始计数 */
    uint32_t getOdoLeft() const { return _odoLeft; }

    /** 获取右轮里程计原始计数 */
    uint32_t getOdoRight() const { return _odoRight; }

    /** 获取电推杆状态 */
    ActuatorState getActuatorState() const { return _actState; }

    /** 获取电推杆状态名称字符串 */
    const char *getActuatorStateName() const;

    /** 电推杆是否正在运动 */
    bool isActuatorMoving() const { return _actState == ActuatorState::ACT_MOVING; }

    /** 获取当前活动电机索引 */
    uint8_t getActiveMotor() const { return _actMotor; }

    /** 获取当前电机方向 */
    bool getActuatorDirection() const { return _actPositive; }

    /** 获取电推杆 TX 计数 */
    uint32_t getActTxCount() const { return _actTxCount; }

    /** 获取电推杆 TX 错误计数 */
    uint32_t getActTxErrorCount() const { return _actTxErrorCount; }

    /**
     * @brief 注册转向修正回调 (在心跳 10ms 任务中调用)
     * @param cb 回调函数指针, nullptr 表示取消
     */
    void setTurnCorrectionCallback(TurnCorrectionCallback cb) { _turnCorrCb = cb; }

#if RNET_ENABLE_OMNI_HEARTBEAT
    /** 获取 Omni 心跳 TX 计数 */
    uint32_t getOmniTxCount() const { return _omniTxCount; }

    /** 获取 Omni 心跳 TX 错误计数 */
    uint32_t getOmniTxErrorCount() const { return _omniTxErrorCount; }
#endif

private:
    /* --- 引脚配置 --- */
    gpio_num_t _pinCanTx;
    gpio_num_t _pinCanRx;

    /* --- 摇杆数据 (由互斥锁保护) --- */
    volatile int8_t _speed;       // 速度 [-100, +100]
    volatile int8_t _turn;        // 转向 [-100, +100]
    SemaphoreHandle_t _dataMutex; // 保护摇杆+电推杆数据的互斥锁

    /* --- 控制器主状态 --- */
    volatile RNetState _state;
    volatile bool _taskRunning;

    /* --- 摇杆统计 --- */
    volatile uint32_t _txCount;
    volatile uint32_t _txErrorCount;

    /* --- 电推杆数据 (由 _dataMutex 保护) --- */
    volatile ActuatorState _actState;  // 电推杆状态机
    volatile uint8_t _actMotor;        // 当前活动电机索引 (0~3)
    volatile bool _actPositive;        // 方向: true=正向(0x80), false=反向(0x40)
    volatile uint32_t _actStopCounter; // STOPPING 状态倒计时 (tick 数)
    volatile uint32_t _actTickCounter; // 50ms 分频计数器

    /* --- 电推杆统计 --- */
    volatile uint32_t _actTxCount;
    volatile uint32_t _actTxErrorCount;


    /* --- FreeRTOS 心跳任务 --- */
    TaskHandle_t _heartbeatTaskHandle;

    /* --- 转向修正回调 --- */
    TurnCorrectionCallback _turnCorrCb = nullptr;

    /* --- 硬件限位保护数据 --- */
    /** 各通道最新的高4位状态缓存 (索引 = 低4位通道号, 初始值 = 0x0C 双向允许) */
    volatile uint8_t _limitState[RNET_LIMIT_CHANNEL_COUNT];
    /** motorIndex -> CAN 通道号 的映射表 (默认 i->i) */
    uint8_t _limitChannelMap[RNET_ACT_MOTOR_MAX + 1];
    /** CAN 接收任务句柄 */
    TaskHandle_t _canRxTaskHandle;
    /** CAN 接收任务运行标志 */
    volatile bool _rxTaskRunning;
    /** 当前座椅菜单选中项索引 (-1=不在菜单, 0~15=菜单第N项) */
    volatile int8_t _seatMenuItem;

    /* --- 从 PM 接收的遥测数据 --- */
    volatile uint8_t  _batteryPct       = 0xFF;  ///< 0xFF = 尚未收到
    volatile uint16_t _motorCurrentRaw  = 0;     ///< 驱动电机电流原始值
    volatile uint32_t _odoLeft          = 0;     ///< 左轮里程计计数
    volatile uint32_t _odoRight         = 0;     ///< 右轮里程计计数

    /* --- 座椅控制模式状态 --- */
    volatile bool     _seatModeActive;     ///< 普通座椅模式激活
    volatile uint32_t _lastSeatMoveMs;     ///< 最后一次 seatMove 的时刻
    volatile bool     _specialModeActive;  ///< 特殊座椅模式激活
    volatile bool     _specialMenuEntered; ///< 已发送进入菜单帧序列
    volatile bool     _specialPrepDone;    ///< 准备阶段完成
    volatile int8_t   _specialTarget;      ///< 目标菜单项索引
    volatile bool     _specialExecActive;  ///< 执行阶段激活
    volatile uint32_t _lastSpecialExecMs;  ///< 最后一次 seatSpecialExec 的时刻
    volatile bool     _navPulseActive;     ///< 菜单导航脉冲进行中
    volatile bool     _navResting;         ///< 菜单导航休息阶段
    volatile uint32_t _navPulseStartMs;    ///< 导航脉冲/休息开始时刻

    /* --- 自主导航状态 --- */
    volatile bool     _navModeActive;      ///< 导航模式激活
    volatile bool     _navRunning;         ///< 导航正在执行 (未暂停)
    volatile uint8_t  _navDest;            ///< 导航目标点索引
    volatile uint32_t _lastNavStartMs;     ///< 最后一次 navStart 的时刻

    /* --- 灯光状态 (由 _dataMutex 保护) --- */
    volatile uint8_t  _lampMask          = 0;    ///< 当前灯光 mask (0=全灭不发送)
    volatile uint8_t  _lampState         = 0;    ///< 当前灯光状态位图
    volatile uint32_t _lampTickCounter   = 0;    ///< 灯光重发分频计数器

    /* --- 喇叭倒计时 (由 _dataMutex 保护) --- */
    volatile uint32_t _hornRemainingTicks = 0;   ///< 剩余鸣叫 tick 数 (0=静音)
    volatile bool     _hornStopSent       = true; ///< 停止帧已发出

    /**
     * @brief 设置灯光状态 (内部辅助)
     * @param mask   控制哪些灯的掩码
     * @param state  各灯的期望状态位图
     * @return true 发送成功
     */
    bool lampSet(uint8_t mask, uint8_t state);

    /**
     * @brief FreeRTOS 心跳任务入口 (静态，通过 pvParameter 传递 this)
     */
    static void heartbeatTask(void *pvParameter);

    /**
     * @brief 发送一帧摇杆/心跳数据
     * @param speed 速度值
     * @param turn  转向值
     * @return true 发送成功
     */
    bool sendJoystickFrame(int8_t speed, int8_t turn);

    /**
     * @brief 发送一帧座椅控制 CAN 帧
     * @param payload 完整的 1 字节载荷 (方向标志 | 电机索引)
     * @return true 发送成功
     */
    bool sendActuatorFrame(uint8_t payload);

    /**
     * @brief 心跳任务内部: 处理电推杆状态机 (每 tick 调用)
     *
     * 使用分频计数器实现 50ms 周期，
     * 处理 MOVING/STOPPING 状态逻辑。
     */
    void handleActuatorTick();

    /**
     * @brief 心跳任务内部: 处理灯光重发 (每 tick 调用)
     *
     * 每 RNET_LAMP_TICK_DIVISOR 个 tick (500ms) 重发一次当前灯光帧，
     * 确保 R-Net 灯光控制器持续收到状态。_lampMask==0 时不发送。
     */
    void handleLampTick();

    /**
     * @brief 心跳任务内部: 处理喇叭倒计时 (每 tick 调用)
     *
     * 每 tick 检查 _hornRemainingTicks，持续发送 hornOn 帧；
     * 到期时发送一次 hornOff 帧停止鸣叫。
     */
    void handleHornTick();

    /**
     * @brief 安装并启动 TWAI/CAN 驱动 (125kbps, 扩展帧)
     * @return true 成功
     */
    bool startTWAI();

    /**
     * @brief 停止并卸载 TWAI/CAN 驱动
     * @return true 成功
     */
    bool stopTWAI();

    /**
     * @brief 钳位摇杆值到 [-100, +100]
     */
    static int8_t clampJoyValue(int val);

    /**
     * @brief 解析一帧限位广播 (0x0C140200)，并在越限时触发保护停止
     *
     * 从 payload 中提取通道号 (低4位) 和状态掩码 (高4位)，
     * 若当前活动通道的运动方向被限位锁止，立即调用 actuatorEmergencyStop()。
     *
     * @param payload  data[0] 原始字节
     */
    void processLimitFrame(uint8_t payload);

    /**
     * @brief 解析座椅菜单选项帧 (0x0C180201)
     *
     * @param b0  data[0] 原始字节
     *            0x2X => 菜单第 X 项; 其他 => 不在菜单 (-1)
     */
    void processMenuSelFrame(uint8_t b0);

    /**
     * @brief FreeRTOS CAN 接收任务入口 (静态)
     *
     * 持续调用 twai_receive() 读取总线帧，识别 0x0C140200 后
     * 转发给 processLimitFrame()。可在此扩展其他 ID 的处理。
     */
    static void canRxTask(void *pvParameter);
};

#endif // RNET_CONTROLLER_H
