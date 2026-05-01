/**
 * @file wheelchair_controller.h
 * @brief 轮椅控制器全局单例 + TCP 控制服务器
 *
 * 架构:
 *   - RNetController 运行在 Core 1，心跳任务优先级最高
 *   - TCP 服务器运行在 Core 0，接收局域网控制指令
 *   - 安全看门狗: 300ms 内未收到持续帧则停止运动
 *   - 单客户端控制权: 同时只有一个 TCP 客户端可控制，新连入踢掉旧连接
 *   - MCP 工具: 语音命令通过 XiaoZhi MCP 框架下达，优先级高于 TCP
 */

#ifndef WHEELCHAIR_CONTROLLER_H
#define WHEELCHAIR_CONTROLLER_H

#include "RNetController.h"
#include <stdint.h>

/** TCP 控制服务器监听端口 */
#define WHEELCHAIR_TCP_PORT     8899

/** TCP 控制服务器鉴权 Token (简单明文，局域网内使用) */
#define WHEELCHAIR_AUTH_TOKEN   "wheelchair"

/** 看门狗超时 (ms): 超过此时间未收到运动指令则停止运动 */
#define WHEELCHAIR_WATCHDOG_MS  300

/**
 * @brief 全局安全状态枚举
 *
 *   NORMAL      — 正常可控
 *   SAFE_STOP   — 安全停止（可被 STOP 指令恢复到 NORMAL）
 *   ERROR       — 系统错误，需重启恢复
 *   EMERGENCY   — 急停（硬按键触发），只有 RESET 指令可清除
 */
typedef enum {
    SAFETY_NORMAL    = 0,
    SAFETY_SAFE_STOP = 1,
    SAFETY_ERROR     = 2,
    SAFETY_EMERGENCY = 3,
} SafetyState;

/**
 * @brief 获取全局 RNetController 实例 (懒初始化单例)
 *
 * 首次调用时创建并调用 begin()。
 * begin() 失败时触发 esp_restart()。
 *
 * @return RNetController* 非空指针
 */
RNetController* GetWheelchairController();

/** 获取当前安全状态 */
SafetyState WheelchairGetSafetyState();

/**
 * @brief 设置安全状态（SAFE_STOP / ERROR / EMERGENCY 时自动停止运动）
 */
void WheelchairSetSafetyState(SafetyState state);

/**
 * @brief 启动 TCP 控制服务器任务 (Core 0, 优先级 5)
 *
 * 必须在网络就绪后调用。
 * TCP 协议 (文本行, \n 结尾):
 *   AUTH <token>         — 鉴权 (首条必须)，新连接踢掉旧控制客户端
 *   FORWARD              — 前进 (当前速度档)
 *   BACKWARD             — 后退
 *   LEFT                 — 左转
 *   RIGHT                — 右转
 *   STOP                 — 立即停止
 *   SPEED:<0-100>        — 设置速度百分比
 *   TILT_UP / TILT_DOWN      — 整体倾斜
 *   RECLINE_UP / RECLINE_DOWN — 靠背
 *   LEGS_UP / LEGS_DOWN      — 腿托
 *   ACT_STOP             — 停止所有电推杆
 *   MODE_DRIVE           — 切换到驾驶模式
 *   MODE_SEAT            — 切换到座椅模式
 *   HORN [ms]            — 喇叭 (可选持续毫秒数)
 *   LIGHT <type>         — 灯光 (left/right/hazard/flood/off)
 *   MOVE <speed> <turn>  — 原始摇杆值 [-100,100]
 *   EMERGENCY_STOP       — 急停 (进入 SAFETY_EMERGENCY)
 *   STATUS               — 查询当前状态
 */
void StartWheelchairTcpServer();

/**
 * @brief 向 McpServer 注册轮椅控制 MCP 工具
 *
 *   wheelchair_drive(direction, speed_percent)
 *   wheelchair_actuator(motor, direction)
 *   wheelchair_mode(mode)
 *   wheelchair_status()
 *   wheelchair_horn(duration_ms)
 *   wheelchair_light(type)
 */
void RegisterWheelchairMcpTools();

/* ======================== 触屏 UI 直接控制 API ======================== */
/**
 * @brief 触屏摇杆直接驱动（绕过 TCP 认证和看门狗）
 *   speed:  -127 ~ +127（正 = 前进）
 *   turn:   -127 ~ +127（正 = 右转）
 * SAFETY_EMERGENCY 时此调用无效。
 */
void WheelchairDirectDrive(int8_t speed, int8_t turn);

/** 停止行驶（摇杆松开时调用） */
void WheelchairDirectStop(void);

/**
 * @brief 直接控制电推杆（按下保持）
 *   motor:    RNET_MOTOR_TILT / RNET_MOTOR_RECLINE / RNET_MOTOR_LEGS
 *   positive: true = 伸展，false = 收缩
 * SAFETY_EMERGENCY 时此调用无效。
 */
void WheelchairDirectActuator(uint8_t motor, bool positive);

/** 停止所有电推杆（按钮松开时调用） */
void WheelchairDirectActuatorStop(void);

/** 获取当前速度百分比 (0-100) */
int8_t WheelchairGetSpeedPct(void);

/** 设置速度百分比 (0-100，超出范围自动截断) */
void   WheelchairSetSpeedPct(int8_t pct);

/**
 * @brief 检查 RNetController 是否处于 ERROR 状态（CAN 总线故障等）
 */
bool WheelchairIsRNetError(void);

#endif // WHEELCHAIR_CONTROLLER_H
