#include "wifi_board.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <wifi_station.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>

#define TAG "gezipai"
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_2
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12

class GezipaiBoard : public WifiBoard
{
private:
    Button boot_button_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_timer_handle_t led_timer_;
    adc_oneshot_unit_handle_t adc1_handle_;
    int last_battery_percentage_;

    // LED定时器回调函数
    static void LedTimerCallback(void *arg)
    {
        static uint8_t timer_count_;
        timer_count_++;
        gpio_set_level(BUILTIN_LED_GPIO, timer_count_ % 2);
    }

    void InitializeCodecI2c()
    {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]()
                             {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            } });
        boot_button_.OnPressDown([this]()
                                 { Application::GetInstance().StartListening(); });
        boot_button_.OnPressUp([this]()
                               { Application::GetInstance().StopListening(); });
    }

    void InitializeTimer()
    {
        esp_timer_create_args_t timer_args = {
            .callback = &LedTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "led_timer",
            .skip_unhandled_events = false,
        };

        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &led_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer_, 1000 * 1000)); // 微秒为单位
    }

    void InitializeBatteryAdc()
    {
        // 简单的 ADC1 初始化
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle_));

        // 配置 ADC 通道
        adc_oneshot_chan_cfg_t config = {
            .atten = BATTERY_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle_, BATTERY_ADC_CHANNEL, &config));
        
        last_battery_percentage_ = 50; // 默认50%
    }

    // 简单的电池电量读取
    int GetSimpleBatteryLevel()
    {
        int adc_raw = 0;
        esp_err_t ret = adc_oneshot_read(adc1_handle_, BATTERY_ADC_CHANNEL, &adc_raw);
        if (ret == ESP_OK) {
            // 简单映射：假设 ADC 值 0-4095 对应 0-100%
            last_battery_percentage_ = (adc_raw * 100) / 4095;
            if (last_battery_percentage_ > 100) last_battery_percentage_ = 100;
            if (last_battery_percentage_ < 0) last_battery_percentage_ = 0;
        }
        return last_battery_percentage_;
    }

public:
    GezipaiBoard() : boot_button_(BOOT_BUTTON_GPIO), last_battery_percentage_(50)
    {
        ESP_LOGI(TAG, "Initializing Gezipai Board");
        // init gpio led
        gpio_config_t led_config = {
            .pin_bit_mask = 1ULL << BUILTIN_LED_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&led_config));

        InitializeCodecI2c();
        InitializeButtons();
        InitializeBatteryAdc();
        InitializeTimer();
    }

    ~GezipaiBoard()
    {
        // 停止定时器
        esp_timer_stop(led_timer_);
        esp_timer_delete(led_timer_);
        
        // 释放ADC资源
        adc_oneshot_del_unit(adc1_handle_);
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                                            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true);
        ESP_LOGI(TAG, "Audio codec initialized successfully");
        return &audio_codec;
    }

    // 简单的电池电量获取方法
    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override
    {
        level = GetSimpleBatteryLevel();
        charging = false;  // 简化处理
        discharging = true;
        return true;
    }
};

DECLARE_BOARD(GezipaiBoard);
