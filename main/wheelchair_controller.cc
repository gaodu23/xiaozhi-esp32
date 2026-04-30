/**
 * @file wheelchair_controller.cc
 * @brief 轮椅控制器单例 + TCP 控制服务器 + MCP 工具注册
 *
 * TCP 协议说明见 wheelchair_controller.h
 */

#include "wheelchair_controller.h"
#include "mcp_server.h"
#include "application.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "Wheelchair"
#define TCP_BUF_SIZE   256
#define TCP_MAX_LISTEN 3

/* ======================== 全局安全状态 ======================== */

static volatile SafetyState s_safety_state = SAFETY_NORMAL;

SafetyState WheelchairGetSafetyState() { return s_safety_state; }

void WheelchairSetSafetyState(SafetyState state)
{
    s_safety_state = state;
    if (state == SAFETY_EMERGENCY || state == SAFETY_SAFE_STOP) {
        if (auto* rnet = GetWheelchairController()) {
            rnet->stop();
            rnet->seatStop();
        }
    }
    ESP_LOGW(TAG, "安全状态 -> %s",
             state == SAFETY_NORMAL    ? "NORMAL" :
             state == SAFETY_SAFE_STOP ? "SAFE_STOP" :
             state == SAFETY_ERROR     ? "ERROR" : "EMERGENCY");
}

/* ======================== RNetController 单例 ======================== */

static RNetController* s_rnet = nullptr;
static SemaphoreHandle_t s_init_mutex = nullptr;

RNetController* GetWheelchairController()
{
    if (!s_init_mutex) {
        s_init_mutex = xSemaphoreCreateMutex();
    }
    if (s_rnet) return s_rnet;

    xSemaphoreTake(s_init_mutex, portMAX_DELAY);
    if (!s_rnet) {
        s_rnet = new RNetController();
        if (!s_rnet->begin()) {
            ESP_LOGE(TAG, "RNetController::begin() 失败, 重启!");
            esp_restart();
        }
        ESP_LOGI(TAG, "RNetController 初始化成功");
    }
    xSemaphoreGive(s_init_mutex);
    return s_rnet;
}

/* ======================== TCP 控制服务器 ======================== */

/** 当前速度百分比 (0-100)，所有客户端共享 */
static volatile int8_t s_drive_speed_pct = 50;

/** 当前持有控制权的 socket，-1 表示无 */
static volatile int s_active_sock = -1;

/** MCP 语音控制是否正在使用（优先级高于 TCP） */
static volatile bool s_mcp_controlling = false;

/** 将百分比映射为 joystick 值 */
static inline int8_t pct_to_joy(int8_t pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

typedef struct {
    int      sock;
    bool     authed;
    uint32_t last_move_ms;
} ClientCtx;

static void handle_client_line(ClientCtx* ctx, char* line, size_t len)
{
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    RNetController* rnet = GetWheelchairController();
    char resp[64];

    /* ---------- 鉴权 ---------- */
    if (!ctx->authed) {
        if (strncmp(line, "AUTH ", 5) == 0 &&
            strcmp(line + 5, WHEELCHAIR_AUTH_TOKEN) == 0)
        {
            int old = s_active_sock;
            if (old != -1 && old != ctx->sock) {
                ESP_LOGW(TAG, "踢出旧客户端 sock=%d", old);
                shutdown(old, SHUT_RDWR);
            }
            s_active_sock = ctx->sock;
            ctx->authed = true;
            snprintf(resp, sizeof(resp), "OK\n");
        } else {
            snprintf(resp, sizeof(resp), "ERR UNAUTHORIZED\n");
        }
        send(ctx->sock, resp, strlen(resp), 0);
        return;
    }

    bool motion_blocked = (s_safety_state == SAFETY_EMERGENCY) || s_mcp_controlling;

    /* ---------- 行驶指令 ---------- */
    if (strcmp(line, "FORWARD") == 0) {
        if (!motion_blocked) {
            rnet->setJoystick(pct_to_joy(s_drive_speed_pct), 0);
            ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        snprintf(resp, sizeof(resp), motion_blocked ? "ERR BLOCKED\n" : "OK\n");

    } else if (strcmp(line, "BACKWARD") == 0) {
        if (!motion_blocked) {
            rnet->setJoystick(-pct_to_joy(s_drive_speed_pct), 0);
            ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        snprintf(resp, sizeof(resp), motion_blocked ? "ERR BLOCKED\n" : "OK\n");

    } else if (strcmp(line, "LEFT") == 0) {
        if (!motion_blocked) {
            rnet->setJoystick(pct_to_joy(s_drive_speed_pct) / 2,
                              -pct_to_joy(s_drive_speed_pct));
            ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        snprintf(resp, sizeof(resp), motion_blocked ? "ERR BLOCKED\n" : "OK\n");

    } else if (strcmp(line, "RIGHT") == 0) {
        if (!motion_blocked) {
            rnet->setJoystick(pct_to_joy(s_drive_speed_pct) / 2,
                              pct_to_joy(s_drive_speed_pct));
            ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        snprintf(resp, sizeof(resp), motion_blocked ? "ERR BLOCKED\n" : "OK\n");

    } else if (strcmp(line, "STOP") == 0) {
        rnet->stop();
        ctx->last_move_ms = 0;
        if (s_safety_state == SAFETY_SAFE_STOP) {
            s_safety_state = SAFETY_NORMAL;
        }
        snprintf(resp, sizeof(resp), "OK\n");

    } else if (strncmp(line, "SPEED:", 6) == 0) {
        int pct = atoi(line + 6);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        s_drive_speed_pct = (int8_t)pct;
        snprintf(resp, sizeof(resp), "OK SPEED=%d\n", pct);

    /* ---------- 低级摇杆（程序化客户端） ---------- */
    } else if (strncmp(line, "MOVE ", 5) == 0) {
        if (!motion_blocked) {
            int speed = 0, turn = 0;
            if (sscanf(line + 5, "%d %d", &speed, &turn) == 2) {
                rnet->setJoystick((int8_t)speed, (int8_t)turn);
                ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
                snprintf(resp, sizeof(resp), "OK\n");
            } else {
                snprintf(resp, sizeof(resp), "ERR BAD_ARGS\n");
            }
        } else {
            snprintf(resp, sizeof(resp), "ERR BLOCKED\n");
        }

    /* ---------- 座椅/电推杆 ---------- */
    } else if (strcmp(line, "TILT_UP") == 0) {
        rnet->seatMove(RNET_MOTOR_TILT, true);
        ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(resp, sizeof(resp), "OK\n");
    } else if (strcmp(line, "TILT_DOWN") == 0) {
        rnet->seatMove(RNET_MOTOR_TILT, false);
        ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(resp, sizeof(resp), "OK\n");
    } else if (strcmp(line, "RECLINE_UP") == 0) {
        rnet->seatMove(RNET_MOTOR_RECLINE, true);
        ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(resp, sizeof(resp), "OK\n");
    } else if (strcmp(line, "RECLINE_DOWN") == 0) {
        rnet->seatMove(RNET_MOTOR_RECLINE, false);
        ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(resp, sizeof(resp), "OK\n");
    } else if (strcmp(line, "LEGS_UP") == 0) {
        rnet->seatMove(RNET_MOTOR_LEGS, true);
        ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(resp, sizeof(resp), "OK\n");
    } else if (strcmp(line, "LEGS_DOWN") == 0) {
        rnet->seatMove(RNET_MOTOR_LEGS, false);
        ctx->last_move_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(resp, sizeof(resp), "OK\n");
    } else if (strcmp(line, "ACT_STOP") == 0) {
        rnet->seatStop();
        snprintf(resp, sizeof(resp), "OK\n");

    /* ---------- 模式切换 ---------- */
    } else if (strcmp(line, "MODE_DRIVE") == 0) {
        ESP_LOGI(TAG, "切换到驾驶模式");
        snprintf(resp, sizeof(resp), "OK MODE=DRIVE\n");
    } else if (strcmp(line, "MODE_SEAT") == 0) {
        ESP_LOGI(TAG, "切换到座椅模式");
        snprintf(resp, sizeof(resp), "OK MODE=SEAT\n");

    /* ---------- 附加功能 ---------- */
    } else if (strncmp(line, "HORN", 4) == 0) {
        int ms = 500;
        if (len > 5) sscanf(line + 5, "%d", &ms);
        if (ms <= 0) ms = 500;
        rnet->hornBeep((uint32_t)ms);
        snprintf(resp, sizeof(resp), "OK\n");

    } else if (strncmp(line, "LIGHT ", 6) == 0) {
        char type[16] = {};
        sscanf(line + 6, "%15s", type);
        if      (strcmp(type, "left")   == 0) rnet->lampLeftOn();
        else if (strcmp(type, "right")  == 0) rnet->lampRightOn();
        else if (strcmp(type, "hazard") == 0) rnet->lampHazardOn();
        else if (strcmp(type, "flood")  == 0) rnet->lampFloodOn();
        else                                   rnet->lampAllOff();
        snprintf(resp, sizeof(resp), "OK\n");

    /* ---------- 安全控制 ---------- */
    } else if (strcmp(line, "EMERGENCY_STOP") == 0) {
        WheelchairSetSafetyState(SAFETY_EMERGENCY);
        snprintf(resp, sizeof(resp), "OK EMERGENCY\n");

    } else if (strcmp(line, "RESET_SAFETY") == 0) {
        if (s_safety_state == SAFETY_EMERGENCY || s_safety_state == SAFETY_SAFE_STOP) {
            s_safety_state = SAFETY_NORMAL;
            snprintf(resp, sizeof(resp), "OK NORMAL\n");
        } else {
            snprintf(resp, sizeof(resp), "OK ALREADY_NORMAL\n");
        }

    /* ---------- 状态查询 ---------- */
    } else if (strcmp(line, "STATUS") == 0) {
        uint8_t bat = rnet->getBatteryPct();
        snprintf(resp, sizeof(resp), "OK SPEED=%d SAFETY=%d BAT=%d\n",
                 (int)s_drive_speed_pct, (int)s_safety_state,
                 bat == 0xFF ? -1 : (int)bat);
    } else {
        snprintf(resp, sizeof(resp), "ERR UNKNOWN_CMD\n");
    }

    send(ctx->sock, resp, strlen(resp), 0);
}

static void tcp_client_task(void* arg)
{
    ClientCtx ctx;
    ctx.sock         = (int)(intptr_t)arg;
    ctx.authed       = false;
    ctx.last_move_ms = 0;

    char buf[TCP_BUF_SIZE];
    char line[TCP_BUF_SIZE];
    int  line_len = 0;

    struct timeval tv = { .tv_sec = 0, .tv_usec = WHEELCHAIR_WATCHDOG_MS * 1000 };
    setsockopt(ctx.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "TCP 客户端连接: sock=%d", ctx.sock);

    while (true) {
        int bytes = recv(ctx.sock, buf, sizeof(buf) - 1, 0);

        if (bytes > 0) {
            for (int i = 0; i < bytes; i++) {
                char c = buf[i];
                if (c == '\n') {
                    line[line_len] = '\0';
                    handle_client_line(&ctx, line, line_len);
                    line_len = 0;
                } else if (line_len < (int)sizeof(line) - 1) {
                    line[line_len++] = c;
                }
            }
        } else if (bytes == 0) {
            break;
        } else {
            // 超时 (EAGAIN): 检查看门狗 & 控制权
            if (ctx.authed) {
                if (ctx.last_move_ms > 0) {
                    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                    if ((now - ctx.last_move_ms) > WHEELCHAIR_WATCHDOG_MS) {
                        GetWheelchairController()->stop();
                        ctx.last_move_ms = 0;
                        ESP_LOGD(TAG, "看门狗超时，运动停止");
                    }
                }
                if (s_active_sock != ctx.sock) {
                    ESP_LOGI(TAG, "控制权已转移，断开 sock=%d", ctx.sock);
                    break;
                }
            }
        }
    }

    if (ctx.authed) {
        RNetController* rnet = GetWheelchairController();
        rnet->stop();
        rnet->seatStop();
        if (s_active_sock == ctx.sock) {
            s_active_sock = -1;
        }
        WheelchairSetSafetyState(SAFETY_SAFE_STOP);
        ESP_LOGI(TAG, "控制客户端断开，进入 SAFE_STOP");
    }

    ESP_LOGI(TAG, "TCP 客户端任务结束: sock=%d", ctx.sock);
    close(ctx.sock);
    vTaskDelete(nullptr);
}

static void tcp_server_task(void* /*arg*/)
{
    int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "socket() 创建失败");
        vTaskDelete(nullptr);
        return;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(WHEELCHAIR_TCP_PORT);

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() 失败，端口 %d", WHEELCHAIR_TCP_PORT);
        close(server_sock);
        vTaskDelete(nullptr);
        return;
    }

    listen(server_sock, TCP_MAX_LISTEN);
    ESP_LOGI(TAG, "TCP 控制服务器已启动，端口 %d", WHEELCHAIR_TCP_PORT);

    while (true) {
        struct sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            ESP_LOGW(TAG, "accept() 失败，继续监听");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        xTaskCreatePinnedToCore(
            tcp_client_task, "WC_Client",
            4096, (void*)(intptr_t)client_sock,
            4, nullptr, 0
        );
    }
}

void StartWheelchairTcpServer()
{
    GetWheelchairController();
    xTaskCreatePinnedToCore(
        tcp_server_task, "WC_TCPSrv",
        4096, nullptr,
        5, nullptr, 0
    );
    ESP_LOGI(TAG, "TCP 控制服务器任务已创建");
}

/* ======================== MCP 工具注册 ======================== */

void RegisterWheelchairMcpTools()
{
    auto& mcp = McpServer::GetInstance();

    /* ---- wheelchair_drive ---- */
    mcp.AddTool(
        "wheelchair_drive",
        "控制轮椅行驶方向。direction: forward/backward/left/right/stop，speed_percent: 速度百分比 (0-100)",
        PropertyList({
            Property("direction",     kPropertyTypeString,  std::string("stop")),
            Property("speed_percent", kPropertyTypeInteger, 50, 0, 100),
        }),
        [](const PropertyList& props) -> ReturnValue {
            std::string dir     = props["direction"].value<std::string>();
            int         spd_pct = props["speed_percent"].value<int>();
            s_drive_speed_pct   = (int8_t)spd_pct;
            s_mcp_controlling   = true;

            auto* rnet = GetWheelchairController();
            if (dir == "stop") {
                rnet->stop();
            } else if (dir == "forward") {
                rnet->setJoystick(pct_to_joy((int8_t)spd_pct), 0);
            } else if (dir == "backward") {
                rnet->setJoystick(-pct_to_joy((int8_t)spd_pct), 0);
            } else if (dir == "left") {
                rnet->setJoystick(pct_to_joy((int8_t)spd_pct) / 2,
                                  -pct_to_joy((int8_t)spd_pct));
            } else if (dir == "right") {
                rnet->setJoystick(pct_to_joy((int8_t)spd_pct) / 2,
                                  pct_to_joy((int8_t)spd_pct));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            rnet->stop();
            s_mcp_controlling = false;
            return std::string("已执行: ") + dir;
        }
    );

    /* ---- wheelchair_actuator ---- */
    mcp.AddTool(
        "wheelchair_actuator",
        "控制座椅电推杆。motor: tilt(整体倾斜)/recline(靠背)/legs(腿托)，direction: up/down/stop",
        PropertyList({
            Property("motor",     kPropertyTypeString, std::string("tilt")),
            Property("direction", kPropertyTypeString, std::string("stop")),
        }),
        [](const PropertyList& props) -> ReturnValue {
            std::string motor = props["motor"].value<std::string>();
            std::string dir   = props["direction"].value<std::string>();
            uint8_t motor_idx = (motor == "recline") ? RNET_MOTOR_RECLINE :
                                (motor == "legs")    ? RNET_MOTOR_LEGS    :
                                                       RNET_MOTOR_TILT;
            auto* rnet = GetWheelchairController();
            if (dir == "stop") {
                rnet->seatStop();
            } else {
                rnet->seatMove(motor_idx, dir == "up");
                vTaskDelay(pdMS_TO_TICKS(500));
                rnet->seatStop();
            }
            return std::string("座椅: ") + motor + " " + dir;
        }
    );

    /* ---- wheelchair_mode ---- */
    mcp.AddTool(
        "wheelchair_mode",
        "切换轮椅控制模式。mode: drive(驾驶模式) / seat(座椅调节模式)",
        PropertyList({
            Property("mode", kPropertyTypeString, std::string("drive")),
        }),
        [](const PropertyList& props) -> ReturnValue {
            std::string mode = props["mode"].value<std::string>();
            ESP_LOGI("Wheelchair", "MCP 切换模式: %s", mode.c_str());
            return std::string("已切换到模式: ") + mode;
        }
    );

    /* ---- wheelchair_status ---- */
    mcp.AddTool(
        "wheelchair_status",
        "查询轮椅当前状态，包含速度档位、安全状态、电池电量",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto* rnet = GetWheelchairController();
            uint8_t bat = rnet->getBatteryPct();
            char buf[128];
            const char* safety_str =
                s_safety_state == SAFETY_NORMAL    ? "正常" :
                s_safety_state == SAFETY_SAFE_STOP ? "安全停止" :
                s_safety_state == SAFETY_ERROR     ? "故障" : "急停";
            if (bat == 0xFF) {
                snprintf(buf, sizeof(buf),
                         "速度档位: %d%%, 安全状态: %s, 电池: 未知",
                         (int)s_drive_speed_pct, safety_str);
            } else {
                snprintf(buf, sizeof(buf),
                         "速度档位: %d%%, 安全状态: %s, 电池: %d%%",
                         (int)s_drive_speed_pct, safety_str, (int)bat);
            }
            return std::string(buf);
        }
    );

    /* ---- wheelchair_horn ---- */
    mcp.AddTool(
        "wheelchair_horn",
        "轮椅喇叭短鸣",
        PropertyList({
            Property("duration_ms", kPropertyTypeInteger, 500, 100, 3000),
        }),
        [](const PropertyList& props) -> ReturnValue {
            int dur = props["duration_ms"].value<int>();
            GetWheelchairController()->hornBeep((uint32_t)dur);
            return std::string("喇叭已响");
        }
    );

    /* ---- wheelchair_light ---- */
    mcp.AddTool(
        "wheelchair_light",
        "控制轮椅灯光。type: left(左转)/right(右转)/hazard(双闪)/flood(照明)/off(关闭)",
        PropertyList({
            Property("type", kPropertyTypeString, std::string("off")),
        }),
        [](const PropertyList& props) -> ReturnValue {
            std::string type = props["type"].value<std::string>();
            auto* rnet = GetWheelchairController();
            if      (type == "left")   rnet->lampLeftOn();
            else if (type == "right")  rnet->lampRightOn();
            else if (type == "hazard") rnet->lampHazardOn();
            else if (type == "flood")  rnet->lampFloodOn();
            else                       rnet->lampAllOff();
            return std::string("灯光已设置: ") + type;
        }
    );

    ESP_LOGI(TAG, "轮椅 MCP 工具已注册 (6 个)");
}

/* ======================== 触屏 UI 直接控制 API ======================== */

void WheelchairDirectDrive(int8_t speed, int8_t turn)
{
    if (s_safety_state == SAFETY_EMERGENCY) return;
    RNetController* rnet = GetWheelchairController();
    rnet->setJoystick(speed, turn);
}

void WheelchairDirectStop(void)
{
    RNetController* rnet = GetWheelchairController();
    rnet->stop();
    if (s_safety_state == SAFETY_SAFE_STOP) {
        s_safety_state = SAFETY_NORMAL;
    }
}

void WheelchairDirectActuator(uint8_t motor, bool positive)
{
    if (s_safety_state == SAFETY_EMERGENCY) return;
    RNetController* rnet = GetWheelchairController();
    rnet->seatMove((int)motor, positive);
}

void WheelchairDirectActuatorStop(void)
{
    RNetController* rnet = GetWheelchairController();
    rnet->seatStop();
}

int8_t WheelchairGetSpeedPct(void)
{
    return s_drive_speed_pct;
}

void WheelchairSetSpeedPct(int8_t pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_drive_speed_pct = pct;
}