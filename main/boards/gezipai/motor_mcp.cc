#include "motor_mcp.h"
#include "settings.h"
#include "mcp_server.h"
#include <esp_log.h>

#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_1
#define LEDC_TIMER LEDC_TIMER_1
#define LEDC_GPIO 4               // 振动电机连接的 GPIO 引脚
#define LEDC_FIXED_FREQUENCY 1000 // PWM 频率
// #define DUTY_MAX 8192 // ESP32 LEDC 13-bit 精度，最大占空比
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT

// 音符持续时间（毫秒）
#define QUARTER_NOTE 400 // 四分音符
#define HALF_NOTE 800    // 二分音符
#define NOTE_GAP 50      // 音符间间隔
#define TAG "Motor"
// 小星星的音符（频率和持续时间）
typedef struct
{
    int freq;     // PWM 频率（Hz），模拟音高
    int duty;     // 占空比，控制强度
    int duration; // 持续时间（毫秒）
} Note;

static Note twinkle[] = {
    // 第一段: C4 C4 G4 G4 A4 A4 G4
    {262, 4096, QUARTER_NOTE},
    {262, 4096, QUARTER_NOTE}, // C4 C4
    {392, 4096, QUARTER_NOTE},
    {392, 4096, QUARTER_NOTE}, // G4 G4
    {440, 4096, QUARTER_NOTE},
    {440, 4096, QUARTER_NOTE}, // A4 A4
    {392, 4096, HALF_NOTE},    // G4

    // 第二段: F4 F4 E4 E4 D4 D4 C4
    {349, 4096, QUARTER_NOTE},
    {349, 4096, QUARTER_NOTE}, // F4 F4
    {330, 4096, QUARTER_NOTE},
    {330, 4096, QUARTER_NOTE}, // E4 E4
    {294, 4096, QUARTER_NOTE},
    {294, 4096, QUARTER_NOTE}, // D4 D4
    {262, 4096, HALF_NOTE},    // C4

    // 第三段: G4 G4 F4 F4 E4 E4 D4
    {392, 4096, QUARTER_NOTE},
    {392, 4096, QUARTER_NOTE}, // G4 G4
    {349, 4096, QUARTER_NOTE},
    {349, 4096, QUARTER_NOTE}, // F4 F4
    {330, 4096, QUARTER_NOTE},
    {330, 4096, QUARTER_NOTE}, // E4 E4
    {294, 4096, HALF_NOTE},    // D4

    // 第四段: G4 G4 F4 F4 E4 E4 D4
    {392, 4096, QUARTER_NOTE},
    {392, 4096, QUARTER_NOTE}, // G4 G4
    {349, 4096, QUARTER_NOTE},
    {349, 4096, QUARTER_NOTE}, // F4 F4
    {330, 4096, QUARTER_NOTE},
    {330, 4096, QUARTER_NOTE}, // E4 E4
    {294, 4096, HALF_NOTE},    // D4

    // 第五段: C4 C4 G4 G4 A4 A4 G4
    {262, 4096, QUARTER_NOTE},
    {262, 4096, QUARTER_NOTE}, // C4 C4
    {392, 4096, QUARTER_NOTE},
    {392, 4096, QUARTER_NOTE}, // G4 G4
    {440, 4096, QUARTER_NOTE},
    {440, 4096, QUARTER_NOTE}, // A4 A4
    {392, 4096, HALF_NOTE},    // G4

    // 第六段: F4 F4 E4 E4 D4 D4 C4
    {349, 4096, QUARTER_NOTE},
    {349, 4096, QUARTER_NOTE}, // F4 F4
    {330, 4096, QUARTER_NOTE},
    {330, 4096, QUARTER_NOTE}, // E4 E4
    {294, 4096, QUARTER_NOTE},
    {294, 4096, QUARTER_NOTE}, // D4 D4
    {262, 4096, HALF_NOTE},    // C4
};

void Motor_massage::set_pwm_freq(int freq)
{
    ledc_set_freq(LEDC_MODE, LEDC_TIMER, freq);
}

// 振动函数
void Motor_massage::vibrate(int freq, int duty, int duration)
{
    ESP_LOGI(TAG, "Vibrating at %d Hz, duty %d for %d ms", freq, duty, duration);
    set_pwm_freq(freq); // 设置振动频率
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
    vTaskDelay(pdMS_TO_TICKS(duration));
    // 停止振动
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
    vTaskDelay(pdMS_TO_TICKS(NOTE_GAP)); // 音符间间隔
}

// 播放小星星
void Motor_massage::play_twinkle()
{
    for (int i = 0; i < sizeof(twinkle) / sizeof(twinkle[0]); i++)
    {
        vibrate(twinkle[i].freq, twinkle[i].duty, twinkle[i].duration);
    }
}

void Motor_massage::InitializeMotor()
{
    ESP_LOGI(TAG, "Configuring PWM for vibration motor...");

    // 配置 LEDC 定时器（固定频率）
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FIXED_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 配置 LEDC 通道
    ledc_channel_config_t ledc_channel = {
        .gpio_num = gpio_num_,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0, // 初始关闭震动
        .hpoint = 0,
        .flags = {.output_invert = 0}};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

Motor_massage::Motor_massage()
{
    // 从设置中读取亮度等级
    InitializeMotor();
    auto &mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.motor_mcp.massage",
                       "主人的手部有点累赶紧给他按摩一下吧",
                       PropertyList(), [this](const PropertyList &properties) -> ReturnValue
                       { 
                        
                        ESP_LOGI(TAG, "Massaging rhythm...");
            play_twinkle();
                        return true; });
}
