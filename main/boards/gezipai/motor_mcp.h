#ifndef MOTOR_MCP_H
#define MOTOR_MCP_H

#include "board.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


class Motor_massage
{
private:
    gpio_num_t gpio_num_ = GPIO_NUM_4;


void set_pwm_freq(int freq) ;
// 振动函数
void vibrate(int freq, int duty, int duration) ;
// 播放小星星
void play_twinkle() ;
void InitializeMotor();
public:
    ~Motor_massage()
    {

    }

    explicit Motor_massage();
};

#endif // LED_STRIP_CONTROL_H