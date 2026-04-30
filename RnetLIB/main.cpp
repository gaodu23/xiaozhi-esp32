/**
 * @file main.cpp
 * @brief 串口控制电轮椅 — 基于 RnetLIB
 *
 * 通过 USB Serial 接收单字符命令，控制轮椅运动、座椅、灯光等。
 * 波特率 115200, 每个字符立即执行，无需回车。
 *
 * ═══════════════════════════════════════════════════════════════
 *                         命令列表
 * ═══════════════════════════════════════════════════════════════
 *
 *   === 摇杆控制 (x,y: -100~+100) ===
 *   w  - 前进    joystickMove(0, 100)
 *   s  - 后退    joystickMove(0, -100)
 *   a  - 左转    joystickMove(-100, 0)
 *   d  - 右转    joystickMove(100, 0)
 *   q  - 左前    joystickMove(-70, 70)
 *   e  - 右前    joystickMove(70, 70)
 *   z  - 左后    joystickMove(-70, -70)
 *   c  - 右后    joystickMove(70, -70)
 *   x  - 停止    joystickMove(0, 0)
 *
 *   === 速度控制 ===
 *   +  - 速度 +25%
 *   -  - 速度 -25%
 *   1  - 速度 25%
 *   2  - 速度 50%
 *   3  - 速度 75%
 *   4  - 速度 100%
 *
 *   === 座椅控制 (电推杆直接驱动, CAN ID 0x08080300) ===
 *   Y  - 座椅升高          motor 0, positive
 *   H  - 座椅降低          motor 0, negative
 *   O  - 靠背后仰          motor 2, positive
 *   L  - 靠背前倾          motor 2, negative
 *   J  - 腿托升高          motor 3, negative
 *   U  - 腿托降低          motor 3, positive
 *   I  - 座椅后倾 (Tilt)   motor 1, positive
 *   K  - 座椅前倾 (Tilt)   motor 1, negative
 *   X  - 停止座椅操作
 *
 *   === 灯光 (toggle 切换, 0C000400#) ===
 *   5  - 左转灯 toggle    0C000400 mask=01 state=01
 *   6  - 右转灯 toggle    0C000400 mask=04 state=04
 *   7  - 双闪   toggle    0C000400 mask=15 state=10
 *   8  - 照明灯 toggle    0C000400 mask=80 state=80
 *   0  - 关闭所有灯      0C000400 mask=FF state=00
 *
 *   === 其他 ===
 *   h/?  - 显示帮助
 *   B    - 蜂鸣 (181C0300)
 *   N    - 喇叭 500ms (0C040400)
 *   p/P  - 打印底盘状态 (电量/里程/电流)
 *   R    - 重启
 */

#include <Arduino.h>
#include "RNetController.h"

/* ======================== 全局变量 ======================== */

RNetController rnet;

static int speedPercent = 50;              // 当前速度百分比 (25/50/75/100)
static bool floodOn     = false;           // 照明灯开关状态
static bool lampLeftOn  = false;           // 左转灯开关状态
static bool lampRightOn = false;           // 右转灯开关状态
static bool lampHazOn   = false;           // 双闪开关状态

/* ======================== 辅助函数 ======================== */

/** 按百分比缩放摇杆值 */
static void joystickMove(int turn, int speed) {
    int8_t s = (int8_t)((speed * speedPercent) / 100);
    int8_t t = (int8_t)((turn * speedPercent) / 100);
    rnet.setJoystick(s, t);
    Serial.printf("[JOY] spd=%d trn=%d (scale %d%%)\n", s, t, speedPercent);
}

/** 打印帮助信息 */
static void printHelp() {
    Serial.println(F(
        "\n"
        "========== 串口控制电轮椅 ==========\n"
        "\n"
        "--- 摇杆 ---\n"
        "  w/s     前进/后退\n"
        "  a/d     左转/右转\n"
        "  q/e     左前/右前\n"
        "  z/c     左后/右后\n"
        "  x       停止\n"
        "\n"
        "--- 速度 ---\n"
        "  +/-     速度 ±25%\n"
        "  1/2/3/4 速度 25%/50%/75%/100%\n"
        "\n"
        "--- 座椅 (电推杆) ---\n"
        "  Y/H     座椅升/降\n"
        "  O/L     靠背后/前\n"
        "  J/U     腿托升/降\n"
        "  I/K     俯仰后/前\n"
        "  X       座椅停止\n"
        "\n"
        "--- 灯光 ---\n"
        "  5/6     左转/右转灯 (toggle)\n"
        "  7       双闪 (toggle)\n"
        "  8       照明灯 (toggle)\n"
        "  0       关闭所有灯\n"
        "\n"
        "--- 底盘状态 ---\n"
        "  p       打印底盘状态 (电量/里程/电流)\n"
        "\n"
        "--- 其他 ---\n"
        "  B       蜂鸣  N  喇叭 (500ms)\n"
        "  h/?     帮助  R  重启\n"
        "====================================\n"
    ));
}

/** 打印底盘实时状态 (电量/电池/里程计) */
static void printStatus() {
    uint8_t  bat = rnet.getBatteryPct();
    uint16_t cur = rnet.getMotorCurrentRaw();
    uint32_t oL  = rnet.getOdoLeft();
    uint32_t oR  = rnet.getOdoRight();

    Serial.println(F("[STATUS] -------- 底盘实时状态 --------"));

    if (bat == 0xFF) {
        Serial.println(F("[STATUS]   电池 : -- (未收到)"));
    } else {
        Serial.printf("[STATUS]   电池 : %3u%%\n", bat);
    }

    Serial.printf("[STATUS]   电流 : %u (raw 0x%04X)\n", (unsigned)cur, (unsigned)cur);
    Serial.printf("[STATUS]   里程 : L=%-10lu  R=%-10lu (pulses)\n",
                  (unsigned long)oL, (unsigned long)oR);
    Serial.println(F("[STATUS] -----------------------------------"));
}

/** 设置速度百分比并提示 */
static void setSpeedPercent(int pct) {
    speedPercent = constrain(pct, 25, 100);
    Serial.printf("[SPD] 速度 = %d%%\n", speedPercent);
}

/* ======================== 命令处理 ======================== */

static void processCommand(char ch) {
    switch (ch) {

    /* ---- 摇杆控制 ---- */
    case 'w': joystickMove(0, 100);    break;  // 前进
    case 's': joystickMove(0, -100);   break;  // 后退
    case 'a': joystickMove(-100, 0);   break;  // 左转
    case 'd': joystickMove(100, 0);    break;  // 右转
    case 'q': joystickMove(-70, 70);   break;  // 左前
    case 'e': joystickMove(70, 70);    break;  // 右前
    case 'z': joystickMove(-70, -70);  break;  // 左后
    case 'c': joystickMove(70, -70);   break;  // 右后
    case 'x': joystickMove(0, 0);      break;  // 停止

    /* ---- 速度控制 ---- */
    case '+': setSpeedPercent(speedPercent + 25);  break;
    case '-': setSpeedPercent(speedPercent - 25);  break;
    case '1': setSpeedPercent(25);   break;
    case '2': setSpeedPercent(50);   break;
    case '3': setSpeedPercent(75);   break;
    case '4': setSpeedPercent(100);  break;

    /* ---- 座椅控制 (电推杆 CAN 0x08080300, 心跳任务 50ms 周期发送) ---- */
    case 'Y': rnet.moveActuator(0, true);  Serial.println("[SEAT] 座椅升高");     break;  // motor0 +
    case 'H': rnet.moveActuator(0, false); Serial.println("[SEAT] 座椅降低");     break;  // motor0 -
    case 'O': rnet.moveActuator(2, true);  Serial.println("[SEAT] 靠背后仰"); break;  // motor2 +
    case 'L': rnet.moveActuator(2, false); Serial.println("[SEAT] 靠背前倾"); break;  // motor2 -
    case 'J': rnet.moveActuator(3, false); Serial.println("[SEAT] 腿托升高");   break;  // motor3 -
    case 'U': rnet.moveActuator(3, true);  Serial.println("[SEAT] 腿托降低");   break;  // motor3 +
    case 'I': rnet.moveActuator(1, true);  Serial.println("[SEAT] 俯仰向后");  break;  // motor1 +
    case 'K': rnet.moveActuator(1, false); Serial.println("[SEAT] 俯仰向前");  break;  // motor1 -
    case 'X': rnet.stopActuator();         Serial.println("[SEAT] 座椅停止");     break;  // idle flush

    /* ---- 灯光控制 (toggle) ---- */
    case '5':
        lampLeftOn = !lampLeftOn;
        if (lampLeftOn) { lampRightOn = false; lampHazOn = false; rnet.lampLeftOn(); }
        else            { rnet.lampAllOff(); }
        Serial.printf("[LAMP] 左转灯 %s\n", lampLeftOn ? "ON" : "OFF");
        break;
    case '6':
        lampRightOn = !lampRightOn;
        if (lampRightOn) { lampLeftOn = false; lampHazOn = false; rnet.lampRightOn(); }
        else             { rnet.lampAllOff(); }
        Serial.printf("[LAMP] 右转灯 %s\n", lampRightOn ? "ON" : "OFF");
        break;
    case '7':
        lampHazOn = !lampHazOn;
        if (lampHazOn) { lampLeftOn = false; lampRightOn = false; rnet.lampHazardOn(); }
        else           { rnet.lampAllOff(); }
        Serial.printf("[LAMP] 双闪 %s\n", lampHazOn ? "ON" : "OFF");
        break;
    case '8':
        floodOn = !floodOn;
        if (floodOn) { rnet.lampFloodOn(); }
        else         { rnet.lampAllOff(); }
        Serial.printf("[LAMP] 照明灯 %s\n", floodOn ? "ON" : "OFF");
        break;
    case '0':
        lampLeftOn = lampRightOn = lampHazOn = floodOn = false;
        rnet.lampAllOff();
        Serial.println("[LAMP] 全灯关闭");
        break;

    /* ---- 底盘状态 ---- */
    case 'p':
    case 'P':
        printStatus();
        break;

    case 'B':
        rnet.buzz();
        Serial.println("[BUZZ] 蜂鸣");
        break;
    case 'N':
        rnet.hornBeep(500);
        Serial.println("[HORN] 喇叭");
        break;
    case 'h':
    case '?':
        printHelp();
        break;
    case 'R':
        Serial.println("[SYS] 重启...");
        delay(500);
        ESP.restart();
        break;

    case '\r':
    case '\n':
        break;  // 忽略回车换行

    default:
        Serial.printf("[ERR] 未知命令: '%c' (0x%02X). 输入 h 查看帮助\n", ch, ch);
        break;
    }
}

/* ======================== Setup / Loop ======================== */

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    delay(1000);

    Serial.println(F("\n=============================="));
    Serial.println(F("  串口控制电轮椅 (RnetLIB)"));
    Serial.println(F("==============================\n"));

    // 初始化 R-Net 控制器
    if (!rnet.begin()) {
        Serial.println(F("[ERR] R-Net 初始化失败!"));
        while (true) delay(1000);
    }


    printHelp();
    Serial.println(F("[SYS] 就绪. 输入命令字符即可控制."));
}

void loop() {
    // 读取串口命令
    while (Serial.available()) {
        char ch = Serial.read();
        processCommand(ch);
    }

    // // 每 2 秒自动打印底盘状态
    // static uint32_t lastStatusMs = 0;
    // uint32_t now = millis();
    // if (now - lastStatusMs >= 2000) {
    //     lastStatusMs = now;
    //     printStatus();
    // }

    delay(10);
}
