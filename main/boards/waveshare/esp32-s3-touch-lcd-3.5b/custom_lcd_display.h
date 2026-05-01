#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include "lcd_display.h"

/**
 * @brief 轮椅控制 LCD 显示界面
 *
 * 布局 (480×320)：
 *   顶栏 (28px)  : 网络/静音/电池 图标 + 状态文字
 *   左面板 (230px): 虚拟摇杆（触屏）+ 小智表情
 *   右面板 (250px): 电推杆按钮 / 模式切换 / 速度调节
 *   底条 (28px)  : IMU 姿态角 + 安全状态
 *   底部弹出层   : 聊天文字（有内容时叠加在底条上）
 */

/** 按钮动作枚举，作为 user_data 传入统一回调 */
enum WcAction : int {
    WCA_TILT_UP    = 0,
    WCA_TILT_DOWN  = 1,
    WCA_RECL_UP    = 2,
    WCA_RECL_DOWN  = 3,
    WCA_LEGS_UP    = 4,
    WCA_LEGS_DOWN  = 5,
    WCA_ACT_STOP   = 6,
    WCA_MODE_DRIVE = 7,
    WCA_MODE_SEAT  = 8,
    WCA_SPD_UP     = 9,
    WCA_SPD_DOWN   = 10,
};

class CustomLcdDisplay : public LcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);

    virtual void SetupUI() override;

private:
    /* ---- 摇杆 ---- */
    lv_obj_t* joy_base_    = nullptr;   ///< 摇杆圆形底座
    lv_obj_t* joy_knob_    = nullptr;   ///< 摇杆旋钮
    lv_obj_t* joy_dir_lbl_ = nullptr;   ///< 摇杆方向文字指示

    /* ---- 状态/速度标签 ---- */
    lv_obj_t* spd_label_   = nullptr;   ///< 当前速度百分比
    lv_obj_t* imu_label_   = nullptr;   ///< IMU 姿态角条
    lv_timer_t* imu_timer_ = nullptr;   ///< 5Hz 定时刷新

    /* ---- 告警覆盖层 ---- */
    lv_obj_t* alert_banner_     = nullptr;  ///< 安全状态告警横幅（EMERGENCY/ERROR 时显示）
    lv_obj_t* alert_label_      = nullptr;  ///< 告警横幅文字
    lv_obj_t* rnet_err_overlay_ = nullptr;  ///< RNetState ERROR 时全屏覆盖层
    lv_obj_t* rnet_err_label_   = nullptr;  ///< 全屏错误文字

    /* ---- LCD 刷新回调（原有）---- */
    static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io,
                                                   esp_lcd_panel_io_event_data_t* edata,
                                                   void* user_ctx);
    static void lvgl_port_flush_callback(lv_display_t* drv,
                                         const lv_area_t* area,
                                         uint8_t* color_map);

    /* ---- 事件回调 ---- */
    static void JoystickEventCb(lv_event_t* e);    ///< 摇杆触摸事件
    static void ControlBtnCb(lv_event_t* e);        ///< 所有控制按钮（WcAction）
    static void ImuTimerCb(lv_timer_t* timer);       ///< IMU 刷新定时器

    /* ---- 辅助：创建带文字的控制按钮 ---- */
    lv_obj_t* MakeCtrlBtn(lv_obj_t* parent, const char* text,
                          int x, int y, int w, int h,
                          WcAction action, bool hold_mode = false);
};

#endif // __CUSTOM_LCD_DISPLAY_H__
