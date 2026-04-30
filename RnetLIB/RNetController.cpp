/**
 * @file RNetController.cpp
 */

#include "RNetController.h"
#include <math.h>

static const char *const TAG = "RNet";

/* ==================== 构造 / 析构 ==================== */

RNetController::RNetController()
    : _pinCanTx(PIN_CAN_TX), _pinCanRx(PIN_CAN_RX), _speed(0), _turn(0), _dataMutex(nullptr), _state(RNetState::IDLE), _taskRunning(false), _txCount(0), _txErrorCount(0), _actState(ActuatorState::ACT_IDLE), _actMotor(0), _actPositive(true), _actStopCounter(0), _actTickCounter(0), _actTxCount(0), _actTxErrorCount(0),
#if RNET_ENABLE_OMNI_HEARTBEAT
      _omniTickCounter(0), _omniTxCount(0), _omniTxErrorCount(0),
#endif
      _heartbeatTaskHandle(nullptr), _canRxTaskHandle(nullptr), _rxTaskRunning(false), _seatMenuItem(-1),
      _seatModeActive(false), _lastSeatMoveMs(0),
      _specialModeActive(false), _specialMenuEntered(false), _specialPrepDone(false),
      _specialTarget(-1), _specialExecActive(false), _lastSpecialExecMs(0),
      _navPulseActive(false), _navResting(false), _navPulseStartMs(0),
      _navModeActive(false), _navRunning(false), _navDest(0), _lastNavStartMs(0),
      _lampMask(0), _lampState(0), _lampTickCounter(0),
      _hornRemainingTicks(0), _hornStopSent(true)
{
    // 初始化为 0x0C: 双向允许，未收到实际帧前不限制运动
    memset((void *)_limitState, 0x0C, sizeof(_limitState));

    for (uint8_t i = 0; i <= RNET_ACT_MOTOR_MAX; i++)
    {
        _limitChannelMap[i] = i;
    }
}

RNetController::~RNetController()
{
    shutdown();
    if (_dataMutex)
    {
        vSemaphoreDelete(_dataMutex);
        _dataMutex = nullptr;
    }
}

/* ==================== 公共方法 ==================== */

bool RNetController::begin()
{
    ESP_LOGI(TAG, "初始化控制器...");

    if (!_dataMutex)
    {
        _dataMutex = xSemaphoreCreateMutex();
        if (!_dataMutex)
        {
            ESP_LOGE(TAG, "互斥锁创建失败");
            _state = RNetState::ERROR;
            return false;
        }
    }

    ESP_LOGI(TAG, "正在启动 TWAI 驱动 (125kbps)...");
    if (!startTWAI())
    {
        ESP_LOGE(TAG, "TWAI 驱动启动失败!");
        _state = RNetState::ERROR;
        return false;
    }

    // 重置摇杆值和统计计数
    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        _speed = 0;
        _turn = 0;
        _actState = ActuatorState::ACT_IDLE;
        _actStopCounter = 0;
        _actTickCounter = 0;
        xSemaphoreGive(_dataMutex);
    }
    _txCount = 0;
    _txErrorCount = 0;
    _actTxCount = 0;
    _actTxErrorCount = 0;
#if RNET_ENABLE_OMNI_HEARTBEAT
    _omniTickCounter = 0;
    _omniTxCount = 0;
    _omniTxErrorCount = 0;
#endif

    sendAttackFrameBurst();

    _taskRunning = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        heartbeatTask, "RNetHB", RNET_TASK_STACK_SIZE,
        this, RNET_TASK_PRIORITY, &_heartbeatTaskHandle, RNET_TASK_CORE
    );

    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "心跳任务创建失败!");
        stopTWAI();
        _state = RNetState::ERROR;
        return false;
    }

    _state = RNetState::RUNNING;
    ESP_LOGI(TAG, "控制器初始化完成, 心跳任务已启动");

    _rxTaskRunning = true;
    ret = xTaskCreatePinnedToCore(
        canRxTask, "RNetRX", 3072,
        this, RNET_TASK_PRIORITY - 1, &_canRxTaskHandle, RNET_TASK_CORE
    );
    if (ret != pdPASS)
    {
        ESP_LOGW(TAG, "CAN接收任务创建失败, 限位保护将不可用!");
        _rxTaskRunning = false;
        _canRxTaskHandle = nullptr;
    }

    return true;
}

void RNetController::setJoystick(int8_t speed, int8_t turn)
{
    int8_t clampedSpeed = (speed > RNET_JOY_MAX) ? RNET_JOY_MAX : (speed < RNET_JOY_MIN) ? RNET_JOY_MIN : speed;
    int8_t clampedTurn  = (turn  > RNET_JOY_MAX) ? RNET_JOY_MAX : (turn  < RNET_JOY_MIN) ? RNET_JOY_MIN : turn;

    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        _speed = clampedSpeed;
        _turn = clampedTurn;
        xSemaphoreGive(_dataMutex);
    }
}

void RNetController::stop()
{
    ESP_LOGW(TAG, "!!! 紧急停止 !!!");

    // 归零摇杆值 + 停止电推杆
    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        _speed = 0;
        _turn = 0;
        // 电推杆也立即停止 (跳过 flush)
        _actState = ActuatorState::ACT_IDLE;
        _actStopCounter = 0;
        xSemaphoreGive(_dataMutex);
    }

    if (_state == RNetState::RUNNING)
    {
        sendJoystickFrame(0, 0);
        sendActuatorFrame(_actMotor);
    }
}

void RNetController::shutdown()
{
    ESP_LOGI(TAG, "系统关闭中...");

    esp_restart(); // 直接重启 ESP32，简化关闭流程，确保所有资源清理和状态重置
}

/* ==================== 座椅/电推杆公共方法 ==================== */

void RNetController::moveActuator(uint8_t motorIndex, bool positive)
{
    uint8_t motor = motorIndex & RNET_ACT_MOTOR_MASK;
    if (motor > RNET_ACT_MOTOR_MAX)
    {
        motor = RNET_ACT_MOTOR_MAX;
    }

    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        _actMotor = motor;
        _actPositive = positive;
        _actState = ActuatorState::ACT_MOVING;
        _actTickCounter = RNET_ACTUATOR_TICK_DIVISOR;
        xSemaphoreGive(_dataMutex);
    }

    ESP_LOGI(TAG, "电推杆开始运动: Motor=%d, 方向=%s",
                  motor, positive ? "正向" : "反向");
}

void RNetController::stopActuator()
{
    uint8_t motor = _actMotor;

    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        _actState = ActuatorState::ACT_IDLE;
        _actStopCounter = 0;
        xSemaphoreGive(_dataMutex);
    }

    if (_state == RNetState::RUNNING)
    {
        sendActuatorFrame(motor); // 发送一帧 idle
    }

    ESP_LOGI(TAG, "电推杆停止: Motor=%d", motor);
}

/* ==================== 硬件限位保护 ==================== */

void RNetController::setLimitChannel(uint8_t motorIndex, uint8_t canChannel)
{
    if (motorIndex > RNET_ACT_MOTOR_MAX)
        return;
    _limitChannelMap[motorIndex] = canChannel & 0x0F; // 只取低4位
}

bool RNetController::canMotorExtend(uint8_t motorIndex) const
{
    if (motorIndex > RNET_ACT_MOTOR_MAX)
        return true; // 索引越界保守返回允许
    uint8_t ch = _limitChannelMap[motorIndex];
    return (_limitState[ch] & RNET_LIMIT_BIT_EXTEND) != 0;
}

bool RNetController::canMotorRetract(uint8_t motorIndex) const
{
    if (motorIndex > RNET_ACT_MOTOR_MAX)
        return true;
    uint8_t ch = _limitChannelMap[motorIndex];
    return (_limitState[ch] & RNET_LIMIT_BIT_RETRACT) != 0;
}

void RNetController::processLimitFrame(uint8_t payload)
{
    // data[0]: 高4位=状态掩码, 低4位=通道号
    uint8_t channel = payload & 0x0F;
    uint8_t state   = (payload >> 4) & 0x0F;
    _limitState[channel] = state;
}

void RNetController::processMenuSelFrame(uint8_t b0)
{
    if ((b0 & 0xF0) == 0x20)
    {
        int8_t idx = (int8_t)(b0 & 0x0F);
        _seatMenuItem = idx;
        ESP_LOGI(TAG, "座椅菜单选项: %d (byte=0x%02X)", idx, b0);
    }
    else
    {
        // _seatMenuItem = -1;
    }
}

void RNetController::canRxTask(void *pvParameter)
{
    RNetController *self = static_cast<RNetController *>(pvParameter);
    twai_message_t msg;

    while (self->_rxTaskRunning)
    {
        esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(20));

        if (err == ESP_ERR_TIMEOUT) continue;

        if (err != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (!msg.extd || msg.rtr) continue;

        // 限位状态广播帧 (0x0C140200)
        if (msg.identifier == RNET_LIMIT_CAN_ID && msg.data_length_code >= 1)
        {
            self->processLimitFrame(msg.data[0]);
            continue;
        }

        // 座椅菜单选项广播帧 (0x0C180201)
        if (msg.identifier == RNET_SEAT_MENU_CAN_ID && msg.data_length_code >= 1)
        {
            self->processMenuSelFrame(msg.data[0]);
            continue;
        }

        // 电池电量帧 (0x1C0C0000)
        if (msg.identifier == RNET_BATTERY_CAN_ID && msg.data_length_code >= 1)
        {
            self->_batteryPct = msg.data[0];
            continue;
        }

        // 驱动电机电流帧 (0x14300000)
        if (msg.identifier == RNET_MOTOR_CURRENT_CAN_ID && msg.data_length_code >= 2)
        {
            self->_motorCurrentRaw = (uint16_t)(msg.data[0] | (msg.data[1] << 8));
            continue;
        }

        // 里程计帧 (0x1C300004)
        if (msg.identifier == RNET_ODOMETER_CAN_ID && msg.data_length_code >= 8)
        {
            self->_odoLeft  = (uint32_t)(msg.data[0]
                            | ((uint32_t)msg.data[1] << 8)
                            | ((uint32_t)msg.data[2] << 16)
                            | ((uint32_t)msg.data[3] << 24));
            self->_odoRight = (uint32_t)(msg.data[4]
                            | ((uint32_t)msg.data[5] << 8)
                            | ((uint32_t)msg.data[6] << 16)
                            | ((uint32_t)msg.data[7] << 24));
            continue;
        }
    }

    vTaskDelete(nullptr);
}

int RNetController::sendAttackFrameBurst()
{
    twai_message_t msg = {};
    msg.identifier = 0x0C000000UL;
    msg.extd = 1;
    msg.rtr = 0;
    msg.data_length_code = 0;

    int queued = 0;
    const TickType_t noWait = 0;
    for (int i = 0; i < 5; i++)
    {
        esp_err_t err = twai_transmit(&msg, noWait);
        if (err == ESP_OK)
        {
            queued++;
        }
    }

    ESP_LOGI(TAG, "AttackBurst: %d/5 帧已入队", queued);
    return queued;
}

void RNetController::actuatorEmergencyStop()
{
    ESP_LOGW(TAG, "电推杆紧急停止!");

    uint8_t motor = _actMotor;

    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        _actState = ActuatorState::ACT_IDLE;
        _actStopCounter = 0;
        xSemaphoreGive(_dataMutex);
    }

    if (_state == RNetState::RUNNING)
    {
        sendActuatorFrame(motor);
    }
}

const char *RNetController::getActuatorStateName() const
{
    switch (_actState)
    {
    case ActuatorState::ACT_IDLE:
        return "IDLE";
    case ActuatorState::ACT_MOVING:
        return "MOVING";
    case ActuatorState::ACT_STOPPING:
        return "STOPPING";
    default:
        return "UNKNOWN";
    }
}

/* ==================== 蜂鸣器 / 菜单 ==================== */

bool RNetController::buzz()
{
    twai_message_t m = {};
    m.identifier = RNET_BUZZ_CAN_ID;
    m.extd = 1; m.rtr = 0; m.data_length_code = 8;
    m.data[0] = 0x02; m.data[1] = 0x60;
    esp_err_t err = twai_transmit(&m, pdMS_TO_TICKS(10));
    ESP_LOGD(TAG, "buzz");
    return (err == ESP_OK);
}

// ================================================================
// 灯光控制
// ================================================================

// 将灯光状态位映射到对应的 Toggle CAN ID
static uint32_t lampStateToCanId(uint8_t state)
{
    if (state & RNET_LAMP_FLOOD)      return RNET_LAMP_FLOOD_CAN_ID;
    if (state & RNET_LAMP_HAZARD)     return RNET_LAMP_HAZARD_CAN_ID;
    if (state & RNET_LAMP_RIGHT_TURN) return RNET_LAMP_RIGHT_CAN_ID;
    if (state & RNET_LAMP_LEFT_TURN)  return RNET_LAMP_LEFT_CAN_ID;
    return 0;
}

// 发送 DLC=0 扩展 CAN 帧 (Toggle 类命令通用)
static bool sendExtFrame0(uint32_t canId)
{
    twai_message_t m = {};
    m.identifier = canId;
    m.extd = 1; m.rtr = 0; m.data_length_code = 0;
    return twai_transmit(&m, pdMS_TO_TICKS(10)) == ESP_OK;
}

bool RNetController::lampSet(uint8_t mask, uint8_t state)
{
    if (state == 0) return lampAllOff();

    uint8_t oldState = 0;
    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        oldState   = _lampState;
        _lampMask  = mask;
        _lampState = state;
        _lampTickCounter = 0;
        xSemaphoreGive(_dataMutex);
    }

    // 若有不同灯光正在亮起, 先发 toggle 将其关闭
    if (oldState != 0 && oldState != state)
        sendExtFrame0(lampStateToCanId(oldState));

    uint32_t canId = lampStateToCanId(state);
    if (!canId) return false;
    return sendExtFrame0(canId);
}

bool RNetController::lampLeftOn()   { return lampSet(RNET_LAMP_LEFT_TURN,  RNET_LAMP_LEFT_TURN);  }
bool RNetController::lampRightOn()  { return lampSet(RNET_LAMP_RIGHT_TURN, RNET_LAMP_RIGHT_TURN); }
bool RNetController::lampHazardOn() { return lampSet(RNET_LAMP_HAZARD | RNET_LAMP_RIGHT_TURN | RNET_LAMP_LEFT_TURN,
                                                     RNET_LAMP_HAZARD); }
bool RNetController::lampFloodOn()  { return lampSet(RNET_LAMP_FLOOD, RNET_LAMP_FLOOD); }

bool RNetController::lampAllOff()
{
    uint8_t lastState = 0;
    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        lastState  = _lampState;
        _lampMask  = 0;
        _lampState = 0;
        _lampTickCounter = 0;
        xSemaphoreGive(_dataMutex);
    }
    if (lastState == 0) return true;
    uint32_t canId = lampStateToCanId(lastState);
    if (!canId) return true;
    return sendExtFrame0(canId);
}

// ================================================================
// 喇叭控制
// ================================================================
bool RNetController::hornOn()  { return sendExtFrame0(RNET_HORN_CAN_ID); }
bool RNetController::hornOff() { return sendExtFrame0(RNET_HORN_STOP_CAN_ID); }

bool RNetController::hornBeep(uint32_t duration_ms)
{
    uint32_t ticks = (duration_ms + 9) / RNET_TX_PERIOD_MS;
    if (ticks == 0) ticks = 1;

    if (xSemaphoreTake(_dataMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        _hornRemainingTicks = ticks;
        _hornStopSent       = false;
        xSemaphoreGive(_dataMutex);
    }
    return hornOn();
}

// bool RNetController::seatMenuEnter()
// {
//     twai_message_t m = {};
//     m.identifier = RNET_SEAT_MENU_CMD_CAN_ID;
//     m.extd = 0; m.rtr = 0; m.data_length_code = 4;

//     m.data[0] = 0x40;
//     esp_err_t err1 = twai_transmit(&m, pdMS_TO_TICKS(10));
//     vTaskDelay(pdMS_TO_TICKS(30));
//     m.data[0] = 0x00; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x00;
//     esp_err_t err2 = twai_transmit(&m, pdMS_TO_TICKS(10));
//     ESP_LOGI(TAG, "seatMenuEnter");
//     return (err1 == ESP_OK && err2 == ESP_OK);
// }

// bool RNetController::seatMenuExit()
// {
//     twai_message_t m = {};
//     m.identifier = RNET_SEAT_MENU_CMD_CAN_ID;
//     m.extd = 0; m.rtr = 0; m.data_length_code = 4;

//     m.data[0] = 0x40; m.data[1] = 0x01;
//     esp_err_t err1 = twai_transmit(&m, pdMS_TO_TICKS(10));
//     vTaskDelay(pdMS_TO_TICKS(50));
//     m.data[0] = 0x00; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x00;
//     esp_err_t err2 = twai_transmit(&m, pdMS_TO_TICKS(10));
//     ESP_LOGI(TAG, "seatMenuExit");
//     return (err1 == ESP_OK && err2 == ESP_OK);
// }

bool RNetController::seatMenuEnter()
{
    twai_message_t m = {};

    m.identifier = 0x00000064;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x40; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x54; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x02;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.data[0] = 0x60; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.data[0] = 0x70; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x09;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.data[0] = 0x80; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x10;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000064;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x00; m.data[1] = 0x10; m.data[2] = 0x00; m.data[3] = 0x02;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x14; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x02;
    twai_transmit(&m, pdMS_TO_TICKS(100));

    vTaskDelay(pdMS_TO_TICKS(100));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x20; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000062;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x30; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x01;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x80; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x80;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x181C0400; // buzz
    m.extd = 1; m.rtr = 0; m.data_length_code = 8;
    m.data[0] = 0x02; m.data[1] = 0x60; m.data[2] = 0x00; m.data[3] = 0x00;
    m.data[4] = 0x00; m.data[5] = 0x00; m.data[6] = 0x00; m.data[7] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "seatMenuEnter");
    return 1;
}

bool RNetController::seatMenuExit()
{
    twai_message_t m = {};

    m.identifier = 0x00000064;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x40; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x54; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x02;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x60; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000062;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x70; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x09;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x80; m.data[1] = 0x01; m.data[2] = 0x00; m.data[3] = 0x10;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000064;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x00; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x14; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x02;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x20; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x30; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x01;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x00000065;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[0] = 0x80; m.data[1] = 0x00; m.data[2] = 0x00; m.data[3] = 0x80;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));



    m.identifier = RNET_SPEED_INFO_CAN_ID;
    m.extd = 1; m.rtr = 0; m.data_length_code = 2;
    m.data[0] = 0x01; m.data[1] = 0x01;
    esp_err_t err4 = twai_transmit(&m, pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(30));

    m.identifier = 0x181C0400; // buzz
    m.extd = 1; m.rtr = 0; m.data_length_code = 8;
    m.data[0] = 0x02; m.data[1] = 0x60; m.data[2] = 0x00; m.data[3] = 0x00;
    m.data[4] = 0x00; m.data[5] = 0x00; m.data[6] = 0x00; m.data[7] = 0x00;
    twai_transmit(&m, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "seatMenuExit");
    return 1;
}

/* ==================== 配置命令 ==================== */

bool RNetController::setSpeed(uint8_t level)
{
    static const uint8_t kSpeedTable[5] = {0x00, 0x19, 0x32, 0x4B, 0x64};
    if (level < 1 || level > 5)
        return false;

    twai_message_t m = {};
    m.identifier = RNET_SPEED_CAN_ID;
    m.extd = 1; m.rtr = 0; m.data_length_code = 1;
    m.data[0] = kSpeedTable[level - 1];
    esp_err_t err = twai_transmit(&m, pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "速度档位 -> %d (0x%02X)", level, kSpeedTable[level - 1]);
    return (err == ESP_OK);
}

bool RNetController::setProfile(uint8_t profile)
{
    if (profile < 1 || profile > 3)
        return false;

    twai_message_t m = {};
    m.identifier = RNET_PROFILE_CAN_ID;
    m.extd = 0; m.rtr = 0; m.data_length_code = 4;
    m.data[1] = (uint8_t)(profile - 1);
    esp_err_t err = twai_transmit(&m, pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "配置文件 -> %d", profile);
    return (err == ESP_OK);
}

/* ==================== 座椅控制模式 ==================== */

void RNetController::seatModeReset()
{
    _seatModeActive     = false;
    _specialModeActive  = false;
    _specialMenuEntered = false;
    _specialPrepDone    = false;
    _specialExecActive  = false;
    _specialTarget      = -1;
    _navPulseActive     = false;
    _navResting         = false;
    stopActuator();
    setJoystick(0, 0);
}

void RNetController::navStart(uint8_t dest)
{
    _navModeActive = true;
    _navRunning = true;
    _navDest = dest;
    _lastNavStartMs = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "导航开始, 目标点: %d", dest);
}

void RNetController::navPause()
{
    if (_navRunning)
    {
        _navRunning = false;
        setJoystick(0, 0);
        ESP_LOGI(TAG, "导航暂停");
    }
}

void RNetController::navResume(uint8_t dest)
{
    if (_navModeActive)
    {
        _navRunning = true;
        _navDest = dest;
        _lastNavStartMs = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "导航恢复, 目标点: %d", dest);
    }
}

void RNetController::navCancel()
{
    ESP_LOGI(TAG, "导航取消");
    navReset();
}

void RNetController::navReset()
{
    _navModeActive = false;
    _navRunning    = false;
    _navDest       = 0;
    setJoystick(0, 0);
}

void RNetController::seatMove(uint8_t motorIndex, bool positive)
{
    _seatModeActive = true;
    _lastSeatMoveMs = (uint32_t)(esp_timer_get_time() / 1000);
    moveActuator(motorIndex, positive);
}

void RNetController::seatStop()
{
    ESP_LOGI(TAG, "普通座椅停止");
    seatModeReset();
}

void RNetController::seatSpecialPrepare(int8_t targetItem)
{
    if (!_specialModeActive)
    {
        _specialModeActive  = true;
        _specialMenuEntered = false;
        _specialPrepDone    = false;
    }
    _specialExecActive  = false;
    _specialTarget = targetItem;
}

void RNetController::seatSpecialExec(int8_t speed)
{
    if (_specialModeActive)
    {
        _specialExecActive = true;
        _lastSpecialExecMs = (uint32_t)(esp_timer_get_time() / 1000);
        setJoystick(speed, 0);
    }
}

void RNetController::seatSpecialExit()
{
    ESP_LOGI(TAG, "特殊座椅退出");
    if (_specialMenuEntered)
    {
        seatMenuExit();
    }
    seatModeReset();
}

void RNetController::seatTick()
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    // --- 普通座椅模式: SEAT_MOVE 超时自动停止 ---
    if (_seatModeActive && (now - _lastSeatMoveMs > RNET_SEAT_CMD_TIMEOUT_MS))
    {
        _seatModeActive = false;
        stopActuator();
        ESP_LOGI(TAG, "SEAT_MOVE 超时, 已停止电推杆");
    }

    // --- 特殊座椅模式: 准备阶段 自动导航菜单 ---
    if (_specialModeActive && !_specialPrepDone)
    {
        if (!_specialMenuEntered)
        {
            seatMenuEnter();
            _specialMenuEntered = true;
            ESP_LOGI(TAG, "特殊模式: 已发送进入座椅菜单");
        }
        else if (!_specialPrepDone)
        {
            // Serial.printf("[Seat] 特殊模式: 当前菜单项 %d, 目标菜单项 %d\n",
            //               _seatMenuItem, _specialTarget);
            int8_t current = _seatMenuItem;
            if (current == _specialTarget)
            {
                setJoystick(0, 0);
                _navPulseActive = false;
                _navResting     = false;
                _specialPrepDone = true;
                ESP_LOGI(TAG, "特殊模式: 已到达目标菜单项 %d, 准备完成", current);
            }
            else if (_navPulseActive)
            {
                // 脉冲活跃阶段: 摇杆保持导航方向 1000ms
                if (now - _navPulseStartMs >= RNET_NAV_PULSE_MS)
                {
                    setJoystick(0, 0);
                    _navPulseActive  = false;
                    _navResting      = true;
                    _navPulseStartMs = now;
                    ESP_LOGD(TAG, "特殊模式: 菜单导航脉冲结束, 当前 %d -> 目标 %d",
                                  current, _specialTarget);
                }
            }
            else if (_navResting)
            {
                // 休息阶段: 摇杆保持 (0,0) 2000ms
                if (now - _navPulseStartMs >= RNET_NAV_REST_MS)
                {
                    _navResting = false;
                    ESP_LOGD(TAG, "特殊模式: 休息结束, 当前 %d -> 目标 %d",
                                  current, _specialTarget);
                }
            }
            else
            {
                // 空闲: 开始新一轮导航脉冲
                int8_t dir = RNET_MENU_NAV_SPEED;
                setJoystick(0, dir);
                _navPulseActive  = true;
                _navPulseStartMs = now;
                ESP_LOGD(TAG, "特殊模式: 菜单导航中... 当前 %d -> 目标 %d",
                              current, _specialTarget);
            }
        }
    }

    // --- 特殊座椅模式: 执行阶段超时停止摇杆 ---
    else if (_specialModeActive && _specialPrepDone && _specialExecActive &&
        (now - _lastSpecialExecMs > RNET_SEAT_CMD_TIMEOUT_MS))
    {
        _specialExecActive = false;
        setJoystick(0, 0);
        ESP_LOGI(TAG, "SEAT_SPECIAL_EXEC 超时, 摇杆已归零");
    }

    // --- 自主导航: NAV_START 超时自动暂停 ---
    if (_navModeActive && _navRunning &&
        (now - _lastNavStartMs > RNET_NAV_CMD_TIMEOUT_MS))
    {
        _navRunning = false;
        setJoystick(0, 0);
        ESP_LOGI(TAG, "导航持续指令超时, 自动暂停");
    }
}

/* ==================== 私有方法 ==================== */

bool RNetController::startTWAI()
{
    /*
     * TWAI 驱动配置:
     *   - Normal 模式 (需要 ACK，即总线上必须有其它节点)
     *   - 125 kbps (R-Net 协议规定)
     *   - 接受所有帧 (不做硬件过滤，后续可软件过滤)
     *
     * 注意: 如果调试时总线上没有其它节点，可切换为 TWAI_MODE_NO_ACK
     */

    // 检查当前 TWAI 状态
    twai_status_info_t status;
    if (twai_get_status_info(&status) == ESP_OK)
    {
        if (status.state == TWAI_STATE_RUNNING)
        {
            ESP_LOGI(TAG, "TWAI 驱动已在运行中");
            return true;
        }
    }

    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(_pinCanTx, _pinCanRx, TWAI_MODE_NORMAL);

    g_config.tx_queue_len = 10;
    g_config.rx_queue_len = 10;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    // 安装驱动
    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "TWAI 驱动已安装 (0x%x)", err);
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "TWAI 驱动安装失败 (0x%x)", err);
        return false;
    }

    // 启动驱动
    err = twai_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "TWAI 驱动启动失败 (0x%x)", err);
        twai_driver_uninstall();
        return false;
    }

    ESP_LOGI(TAG, "TWAI 驱动已启动 (125kbps, Normal Mode)");
    return true;
}

bool RNetController::stopTWAI()
{
    esp_err_t err;

    err = twai_stop();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "TWAI 停止失败 (0x%x)", err);
    }

    err = twai_driver_uninstall();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "TWAI 卸载失败 (0x%x)", err);
        return false;
    }

    ESP_LOGI(TAG, "TWAI 驱动已停止并卸载");
    return true;
}

bool RNetController::sendJoystickFrame(int8_t speed, int8_t turn)
{
    /*
     * R-Net 摇杆控制帧格式:
     *
     *   CAN ID:  0x02000300 (29-bit 扩展帧)
     *   DLC:     2
     *   Data[0]: Turn  — 有符号 int8, [-100(左) ~ +100(右)]
     *   Data[1]: Speed — 有符号 int8, [-100(后) ~ +100(前)]
     *
     * 即使摇杆在中位 (0,0)，也必须每 10ms 发送此帧作为心跳。
     * 如果轮椅控制器在约 30ms 内未收到此帧，将触发紧急制动。
     */

    twai_message_t msg = {};
    msg.identifier = RNET_JOYSTICK_CAN_ID;
    msg.extd = 1; // 29-bit 扩展帧
    msg.rtr = 0;  // 数据帧，非远程帧
    msg.data_length_code = RNET_JOYSTICK_DLC;
    msg.data[0] = (uint8_t)turn;  // Byte 0: 转向
    msg.data[1] = (uint8_t)speed; // Byte 1: 速度

    // 使用较短的超时 (5ms)，避免阻塞超过半个心跳周期
    esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(5));
    if (err == ESP_OK)
    {
        _txCount++;
        return true;
    }
    else
    {
        _txErrorCount++;
        // 仅在错误不太频繁时打印，避免刷屏
        if ((_txErrorCount % 100) == 1)
        {
            ESP_LOGW(TAG, "TX 错误 (0x%x), 累计: %lu", err, _txErrorCount);
            twai_status_info_t status;
            if (twai_get_status_info(&status) == ESP_OK)
            {
                ESP_LOGW(TAG, "TWAI 状态: state=%d, tx_err=%lu, rx_err=%lu, tx_failed=%lu, rx_miss=%lu, arb_lost=%lu, bus_err=%lu",
                              status.state, status.tx_error_counter, status.rx_error_counter,
                              status.tx_failed_count, status.rx_missed_count,
                              status.arb_lost_count, status.bus_error_count);
            }
        }

        // *      - ESP_OK: Transmission successfully queued/initiated
        // *      - ESP_ERR_INVALID_ARG: Arguments are invalid
        // *      - ESP_ERR_TIMEOUT: Timed out waiting for space on TX queue
        // *      - ESP_FAIL: TX queue is disabled and another message is currently transmitting
        // *      - ESP_ERR_INVALID_STATE: TWAI driver is not in running state, or is not installed
        // *      - ESP_ERR_NOT_SUPPORTED: Listen Only Mode does not support transmissions
        return false;
    }
}

int8_t RNetController::clampJoyValue(int val)
{
    if (val < RNET_JOY_MIN)
        return (int8_t)RNET_JOY_MIN;
    if (val > RNET_JOY_MAX)
        return (int8_t)RNET_JOY_MAX;
    return (int8_t)val;
}

/* ==================== 座椅帧发送与状态机 ==================== */

bool RNetController::sendActuatorFrame(uint8_t payload)
{
    // CAN ID 0x08080300, DLC=1, payload=[方向位|电机索引]
    twai_message_t msg = {};
    msg.identifier = RNET_ACTUATOR_CAN_ID;
    msg.extd = 1;
    msg.rtr = 0;
    msg.data_length_code = RNET_ACTUATOR_DLC;
    msg.data[0] = payload;

    esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(10));

    if (err == ESP_OK)
    {
        _actTxCount++;
        return true;
    }
    else
    {
        _actTxErrorCount++;
        if ((_actTxErrorCount % 50) == 1)
        {
            ESP_LOGW(TAG, "Actuator TX 错误 (0x%x), payload=0x%02X, 累计: %lu",
                          err, payload, _actTxErrorCount);
        }
        return false;
    }
}

void RNetController::handleActuatorTick()
{
    if (_actState == ActuatorState::ACT_IDLE) return;

    // 每 RNET_ACTUATOR_TICK_DIVISOR 个 tick 发一帧 (50ms)
    _actTickCounter++;
    if (_actTickCounter < RNET_ACTUATOR_TICK_DIVISOR)
    {
        return;
    }
    _actTickCounter = 0;

    switch (_actState)
    {

    case ActuatorState::ACT_MOVING:
    {
        // 发送带方向标志的控制帧
        uint8_t dirFlag = _actPositive ? RNET_ACT_DIR_POSITIVE : RNET_ACT_DIR_NEGATIVE;
        uint8_t payload = dirFlag | _actMotor;
        sendActuatorFrame(payload);
        break;
    }

    case ActuatorState::ACT_STOPPING:
    {
        // 发送 idle 帧 (仅电机索引，无方向位)
        sendActuatorFrame(_actMotor);

        // 每发一帧递减倒计时 (按 50ms 帧计)
        if (_actStopCounter > 0)
        {
            _actStopCounter--;
        }
        if (_actStopCounter == 0)
        {
            _actState = ActuatorState::ACT_IDLE;
            ESP_LOGI(TAG, "电推杆 idle 刷新完成, Motor=%d -> IDLE", _actMotor);
        }
        break;
    }

    default:
        break;
    }
}

void RNetController::handleLampTick()
{
    // 灯光为 Toggle 命令, 不做周期重发
}

void RNetController::handleHornTick()
{
    if (_hornRemainingTicks == 0) return;

    if (--_hornRemainingTicks > 0)
    {
        hornOn(); // 持续鸣叫：每 tick 发送开启帧
    }
    else if (!_hornStopSent)
    {
        _hornStopSent = true;
        hornOff();
        ESP_LOGI(TAG, "喇叭停止");
    }
}

/* ==================== FreeRTOS 心跳任务 ===================*/

void RNetController::heartbeatTask(void *pvParameter)
{
    RNetController *self = static_cast<RNetController *>(pvParameter);
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(RNET_TX_PERIOD_MS);

    ESP_LOGI(TAG, "心跳任务已启动 (10ms 周期)");

    while (self->_taskRunning)
    {
        // 读取当前摇杆值
        int8_t speed = 0;
        int8_t turn = 0;

        if (xSemaphoreTake(self->_dataMutex, pdMS_TO_TICKS(2)) == pdTRUE)
        {
            speed = self->_speed;
            turn = self->_turn;

            // 在同一个临界区内处理电推杆状态机 (50ms 分频)
            self->handleActuatorTick();

            // 在同一个临界区内处理灯光周期重发 (500ms 分频)
            self->handleLampTick();

            // 在同一个临界区内处理喇叭倒计时 (10ms 精度)
            self->handleHornTick();

            xSemaphoreGive(self->_dataMutex);
        }
        // 如果获取互斥锁超时，使用默认值 (0,0)，安全优先

        // 转向修正回调
        if (self->_turnCorrCb)
        {
            turn = self->_turnCorrCb(speed, turn, RNET_TX_PERIOD_MS * 0.001f);
        }

        // 圆形限幅
        {
            float fs = (float)speed;
            float ft = (float)turn;
            float mag = sqrtf(fs * fs + ft * ft);
            if (mag > (float)RNET_JOY_MAX)
            {
                float scale = (float)RNET_JOY_MAX / mag;
                speed = (int8_t)(fs * scale);
                turn  = (int8_t)(ft * scale);
            }
        }

        // 发送摇杆/心跳帧
        self->sendJoystickFrame(speed, turn);

        // 精确延时到下一个 10ms 边界
        vTaskDelayUntil(&xLastWakeTime, xPeriod);
    }

    // 任务退出前，发送最后一帧停止指令
    ESP_LOGI(TAG, "心跳任务即将退出，发送最终停止帧...");
    self->sendJoystickFrame(0, 0);

    // 排除 vTaskDelete 重复
    self->_heartbeatTaskHandle = nullptr;
    vTaskDelete(NULL);
}
