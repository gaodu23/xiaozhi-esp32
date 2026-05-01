
#include "config.h"
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "hwt101_sensor.h"
#include "qmi8658_sensor.h"
#include "wheelchair_controller.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "board.h"

#include <math.h>

#include <vector>
#include <cstring>

#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>

#define TAG "CustomLcdDisplay"


static SemaphoreHandle_t trans_done_sem = NULL;
static uint16_t *trans_act;
static uint16_t *trans_buf_1;
static uint16_t *trans_buf_2;

bool CustomLcdDisplay::lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t taskAwake = pdFALSE;
    lv_display_t *disp_drv = (lv_display_t *)user_ctx;
    assert(disp_drv != NULL);
    if (trans_done_sem) {
        xSemaphoreGiveFromISR(trans_done_sem, &taskAwake);
    }
    return false;
}

void CustomLcdDisplay::lvgl_port_flush_callback(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
    assert(drv != NULL);
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_driver_data(drv);
    assert(panel_handle != NULL);

    size_t len = lv_area_get_size(area);
    lv_draw_sw_rgb565_swap(color_map, len);

    const int x_start = area->x1;
    const int x_end = area->x2;
    const int y_start = area->y1;
    const int y_end = area->y2;
    const int width = x_end - x_start + 1;
    const int height = y_end - y_start + 1;
    
    int32_t hor_res = lv_display_get_horizontal_resolution(drv);
    int32_t ver_res = lv_display_get_vertical_resolution(drv);

    // printf("hor_res: %ld, ver_res: %ld\r\n", hor_res, ver_res);
    // printf("x_start: %d, x_end: %d, y_start: %d, y_end: %d, width: %d, height: %d\r\n", x_start, x_end, y_start, y_end, width, height);
    uint16_t *from = (uint16_t *)color_map;
    uint16_t *to = NULL;

    if (DISPLAY_TRANS_SIZE > 0) {
        assert(trans_buf_1 != NULL);

        int x_draw_start = 0;
        int x_draw_end = 0;
        int y_draw_start = 0;
        int y_draw_end = 0;
        int trans_count = 0;

        trans_act = trans_buf_1;
        lv_display_rotation_t rotate = LV_DISPLAY_ROTATION;

        int x_start_tmp = 0;
        int x_end_tmp = 0;
        int max_width = 0;
        int trans_width = 0;

        int y_start_tmp = 0;
        int y_end_tmp = 0;
        int max_height = 0;
        int trans_height = 0;

        if (LV_DISPLAY_ROTATION_270 == rotate || LV_DISPLAY_ROTATION_90 == rotate) {
            max_width = ((DISPLAY_TRANS_SIZE / height) > width) ? (width) : (DISPLAY_TRANS_SIZE / height);
            trans_count = width / max_width + (width % max_width ? (1) : (0));

            x_start_tmp = x_start;
            x_end_tmp = x_end;
        } else {
            max_height = ((DISPLAY_TRANS_SIZE / width) > height) ? (height) : (DISPLAY_TRANS_SIZE / width);
            trans_count = height / max_height + (height % max_height ? (1) : (0));

            y_start_tmp = y_start;
            y_end_tmp = y_end;
        }

        for (int i = 0; i < trans_count; i++) {

            if (LV_DISPLAY_ROTATION_90 == rotate) {
                trans_width = (x_end - x_start_tmp + 1) > max_width ? max_width : (x_end - x_start_tmp + 1);
                x_end_tmp = (x_end - x_start_tmp + 1) > max_width ? (x_start_tmp + max_width - 1) : x_end;
            } else if (LV_DISPLAY_ROTATION_270 == rotate) {
                trans_width = (x_end_tmp - x_start + 1) > max_width ? max_width : (x_end_tmp - x_start + 1);
                x_start_tmp = (x_end_tmp - x_start + 1) > max_width ? (x_end_tmp - trans_width + 1) : x_start;
            } else if (LV_DISPLAY_ROTATION_0 == rotate) {
                trans_height = (y_end - y_start_tmp + 1) > max_height ? max_height : (y_end - y_start_tmp + 1);
                y_end_tmp = (y_end - y_start_tmp + 1) > max_height ? (y_start_tmp + max_height - 1) : y_end;
            } else {
                trans_height = (y_end_tmp - y_start + 1) > max_height ? max_height : (y_end_tmp - y_start + 1);
                y_start_tmp = (y_end_tmp - y_start + 1) > max_height ? (y_end_tmp - max_height + 1) : y_start;
            }

            trans_act = (trans_act == trans_buf_1) ? (trans_buf_2) : (trans_buf_1);
            to = trans_act;

            switch (rotate) {
            case LV_DISPLAY_ROTATION_90:
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < trans_width; x++) {
                        *(to + x * height + (height - y - 1)) = *(from + y * width + x_start_tmp + x);
                    }
                }
                x_draw_start = ver_res - y_end - 1;
                x_draw_end = ver_res - y_start - 1;
                y_draw_start = x_start_tmp;
                y_draw_end = x_end_tmp;
                break;
            case LV_DISPLAY_ROTATION_270:
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < trans_width; x++) {
                        *(to + (trans_width - x - 1) * height + y) = *(from + y * width + x_start_tmp + x);
                    }
                }
                x_draw_start = y_start;
                x_draw_end = y_end;
                y_draw_start = hor_res - x_end_tmp - 1;
                y_draw_end = hor_res - x_start_tmp - 1;
                break;
            case LV_DISPLAY_ROTATION_180:
                for (int y = 0; y < trans_height; y++) {
                    for (int x = 0; x < width; x++) {
                        *(to + (trans_height - y - 1)*width + (width - x - 1)) = *(from + y_start_tmp * width + y * (width) + x);
                    }
                }
                x_draw_start = hor_res - x_end - 1;
                x_draw_end = hor_res - x_start - 1;
                y_draw_start = ver_res - y_end_tmp - 1;
                y_draw_end = ver_res - y_start_tmp - 1;
                break;
            case LV_DISPLAY_ROTATION_0:
                for (int y = 0; y < trans_height; y++) {
                    for (int x = 0; x < width; x++) {
                        *(to + y * (width) + x) = *(from + y_start_tmp * width + y * (width) + x);
                    }
                }
                x_draw_start = x_start;
                x_draw_end = x_end;
                y_draw_start = y_start_tmp;
                y_draw_end = y_end_tmp;
                break;
            default:
                break;
            }

            if (0 == i) {
                // if (disp_ctx->draw_wait_cb) {
                //     disp_ctx->draw_wait_cb(disp_ctx->panel_handle->user_data);
                // }
                xSemaphoreGive(trans_done_sem);
            }

            xSemaphoreTake(trans_done_sem, portMAX_DELAY);
            // printf("i: %d, x_draw_start: %d, x_draw_end: %d, y_draw_start: %d, y_draw_end: %d\r\n", i, x_draw_start, x_draw_end, y_draw_start, y_draw_end);
            esp_lcd_panel_draw_bitmap(panel_handle, x_draw_start, y_draw_start, x_draw_end + 1, y_draw_end + 1, to);

            if (LV_DISPLAY_ROTATION_90 == rotate) {
                x_start_tmp += max_width;
            } else if (LV_DISPLAY_ROTATION_270 == rotate) {
                x_end_tmp -= max_width;
            } if (LV_DISPLAY_ROTATION_0 == rotate) {
                y_start_tmp += max_height;
            } else {
                y_end_tmp -= max_height;
            }
        }
    } else {
        esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end + 1, y_end + 1, color_map);
    }
    lv_disp_flush_ready(drv);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    //     width_ = width;
    // height_ = height;

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    trans_done_sem = xSemaphoreCreateCounting(1, 0);
    trans_buf_1 = (uint16_t *)heap_caps_malloc(DISPLAY_TRANS_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);
    trans_buf_2 = (uint16_t *)heap_caps_malloc(DISPLAY_TRANS_SIZE * sizeof(uint16_t), MALLOC_CAP_DMA);
#if 0
    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 0,
            .buff_spiram = 1,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 1,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    lv_display_set_flush_cb(display_, lvgl_port_flush_callback);
#else

    uint32_t buffer_size = 0;
    lv_color_t *buf1 = NULL;
    lvgl_port_lock(0);
    uint8_t color_bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    display_ = lv_display_create(width_, height_);
    lv_display_set_flush_cb(display_, lvgl_port_flush_callback);
    buffer_size = width_ * height_;
    buf1 = (lv_color_t *)heap_caps_aligned_alloc(1, buffer_size * color_bytes, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(display_, buf1, NULL, buffer_size * color_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_driver_data(display_, panel_);
    lvgl_port_unlock();

#endif

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_port_flush_io_ready_callback,
    };
    /* Register done callback */
    esp_lcd_panel_io_register_event_callbacks(panel_io_, &cbs, display_);

    esp_lcd_panel_disp_on_off(panel_, false);

    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

/* ================================================================
 * 布局常量 (480 × 320 横屏)
 * ================================================================ */
#define TOP_BAR_H    28     ///< 顶栏高度
#define IMU_BAR_H    28     ///< 底部 IMU 条高度
#define MAIN_H       264    ///< 主区域高度 = 320 - 28 - 28
#define IMU_BAR_Y    292    ///< IMU 条起始 y

#define JOY_PANEL_W  230    ///< 左侧摇杆面板宽度
#define CTRL_X       230    ///< 右侧控制面板起始 x
#define CTRL_W       250    ///< 右侧控制面板宽度

/* 摇杆圆形尺寸 */
#define JOY_DIA      210    ///< 摇杆底座直径
#define JOY_KNOB_D   52     ///< 摇杆旋钮直径
#define JOY_X        ((JOY_PANEL_W - JOY_DIA) / 2)   // = 10
#define JOY_Y        (TOP_BAR_H + (MAIN_H - JOY_DIA) / 2) // = 55

/* 控制按钮行列（屏幕绝对坐标） */
#define CBTN_Y0     (TOP_BAR_H + 5)           // = 33  第1行 (UP buttons)
#define CBTN_H_ACT  55                          // 电推杆按钮高度
#define CBTN_Y1     (CBTN_Y0 + CBTN_H_ACT + 2) // = 90  第2行 (DOWN buttons)
#define CBTN_W_ACT  82                          // 每列宽度 (250/3 ≈ 82)
#define CBTN_Y2     (CBTN_Y1 + CBTN_H_ACT + 4) // = 149 ACT STOP
#define CBTN_H_STD  46                          // 标准按钮高度
#define CBTN_Y3     (CBTN_Y2 + CBTN_H_STD + 3) // = 198 模式按钮行
#define CBTN_Y4     (CBTN_Y3 + CBTN_H_STD + 3) // = 247 速度按钮行

/* 颜色分区 */
#define CLR_BG_DARK  lv_color_hex(0x0C1A2E)  ///< 深蓝背景
#define CLR_BG_PANEL lv_color_hex(0x112233)  ///< 面板背景
#define CLR_BTN_NORM lv_color_hex(0x1B3A5C)  ///< 普通按钮
#define CLR_BTN_PRES lv_color_hex(0x2E6A9E)  ///< 按下状态
#define CLR_BTN_STOP lv_color_hex(0x6B1A1A)  ///< STOP 按钮
#define CLR_BTN_DRIV lv_color_hex(0x1A4A1A)  ///< DRIVE 模式
#define CLR_BTN_SEAT lv_color_hex(0x1A1A4A)  ///< SEAT 模式
#define CLR_JOY_BASE lv_color_hex(0x1A3050)  ///< 摇杆底座
#define CLR_JOY_KNOB lv_color_hex(0x4090D0)  ///< 摇杆旋钮
#define CLR_TEXT     lv_color_hex(0xD0E8FF)  ///< 文字颜色
#define CLR_IMU_TEXT lv_color_hex(0x00FF88)  ///< IMU 绿色文字
#define CLR_BORDER   lv_color_hex(0x2A5A8C)  ///< 边框颜色

/* ================================================================
 * 辅助：创建控制按钮
 * ================================================================ */
lv_obj_t* CustomLcdDisplay::MakeCtrlBtn(lv_obj_t* parent, const char* text,
                                         int x, int y, int w, int h,
                                         WcAction action, bool hold_mode)
{
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, CLR_BTN_NORM, 0);
    lv_obj_set_style_bg_color(btn, CLR_BTN_PRES, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, CLR_BORDER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 2, 0);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, CLR_TEXT, 0);

    void* ud = (void*)(intptr_t)action;
    if (hold_mode) {
        /* 长按：PRESSING 保持发送，RELEASED/PRESS_LOST 停止 */
        lv_obj_add_event_cb(btn, ControlBtnCb, LV_EVENT_PRESSED,    ud);
        lv_obj_add_event_cb(btn, ControlBtnCb, LV_EVENT_PRESSING,   ud);
        lv_obj_add_event_cb(btn, ControlBtnCb, LV_EVENT_RELEASED,   ud);
        lv_obj_add_event_cb(btn, ControlBtnCb, LV_EVENT_PRESS_LOST, ud);
    } else {
        /* 点击：CLICKED 单次触发 */
        lv_obj_add_event_cb(btn, ControlBtnCb, LV_EVENT_CLICKED,    ud);
    }
    return btn;
}

/* ================================================================
 * SetupUI —— 主界面布局
 * ================================================================ */
void CustomLcdDisplay::SetupUI()
{
    /* 1. 先初始化基类 UI（顶栏、状态、表情、聊天等） */
    LcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    auto* scr = lv_screen_active();

    /* ---- 压低表情栏 z-order，让控制面板在它上面 ---- */
    if (emoji_box_) lv_obj_set_pos(emoji_box_, 5, TOP_BAR_H + 4);

    /* ====================================================================
     * 2. 左侧摇杆面板背景
     * ==================================================================== */
    lv_obj_t* left_panel = lv_obj_create(scr);
    lv_obj_set_pos(left_panel, 0, TOP_BAR_H);
    lv_obj_set_size(left_panel, JOY_PANEL_W, MAIN_H);
    lv_obj_set_style_bg_color(left_panel, CLR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(left_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(left_panel, 0, 0);
    lv_obj_set_style_radius(left_panel, 0, 0);
    lv_obj_set_style_pad_all(left_panel, 0, 0);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 分割线 */
    lv_obj_t* divider = lv_obj_create(scr);
    lv_obj_set_pos(divider, JOY_PANEL_W - 1, TOP_BAR_H);
    lv_obj_set_size(divider, 2, MAIN_H);
    lv_obj_set_style_bg_color(divider, CLR_BORDER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);

    /* ====================================================================
     * 3. 虚拟摇杆
     * ==================================================================== */
    joy_base_ = lv_obj_create(scr);
    lv_obj_set_pos(joy_base_, JOY_X, JOY_Y);
    lv_obj_set_size(joy_base_, JOY_DIA, JOY_DIA);
    lv_obj_set_style_radius(joy_base_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(joy_base_, CLR_JOY_BASE, 0);
    lv_obj_set_style_bg_opa(joy_base_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(joy_base_, 3, 0);
    lv_obj_set_style_border_color(joy_base_, CLR_BORDER, 0);
    lv_obj_set_style_pad_all(joy_base_, 0, 0);
    lv_obj_set_style_shadow_width(joy_base_, 8, 0);
    lv_obj_set_style_shadow_color(joy_base_, CLR_BORDER, 0);
    lv_obj_set_style_shadow_opa(joy_base_, LV_OPA_50, 0);
    lv_obj_clear_flag(joy_base_, LV_OBJ_FLAG_SCROLLABLE);

    /* 摇杆十字刻度线 */
    lv_obj_t* hline = lv_obj_create(joy_base_);
    lv_obj_set_size(hline, JOY_DIA - 20, 1);
    lv_obj_set_style_bg_color(hline, lv_color_hex(0x2A5070), 0);
    lv_obj_set_style_border_width(hline, 0, 0);
    lv_obj_set_style_radius(hline, 0, 0);
    lv_obj_center(hline);

    lv_obj_t* vline = lv_obj_create(joy_base_);
    lv_obj_set_size(vline, 1, JOY_DIA - 20);
    lv_obj_set_style_bg_color(vline, lv_color_hex(0x2A5070), 0);
    lv_obj_set_style_border_width(vline, 0, 0);
    lv_obj_set_style_radius(vline, 0, 0);
    lv_obj_center(vline);

    /* 方向文字提示 (↑ ↓ ← →) */
    const char* dir_chars[] = { LV_SYMBOL_UP, LV_SYMBOL_DOWN,
                                 LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT };
    int dir_x[] = { 0, 0, -(JOY_DIA/2 - 14), (JOY_DIA/2 - 14) };
    int dir_y[] = { -(JOY_DIA/2 - 14), (JOY_DIA/2 - 14), 0, 0 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t* dl = lv_label_create(joy_base_);
        lv_label_set_text(dl, dir_chars[i]);
        lv_obj_set_style_text_color(dl, lv_color_hex(0x3A6090), 0);
        lv_obj_align(dl, LV_ALIGN_CENTER, dir_x[i], dir_y[i]);
    }

    /* 旋钮 */
    joy_knob_ = lv_obj_create(joy_base_);
    lv_obj_set_size(joy_knob_, JOY_KNOB_D, JOY_KNOB_D);
    lv_obj_set_style_radius(joy_knob_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(joy_knob_, CLR_JOY_KNOB, 0);
    lv_obj_set_style_border_width(joy_knob_, 2, 0);
    lv_obj_set_style_border_color(joy_knob_, lv_color_hex(0x70C0FF), 0);
    lv_obj_set_style_shadow_width(joy_knob_, 6, 0);
    lv_obj_set_style_shadow_color(joy_knob_, lv_color_hex(0x2060A0), 0);
    lv_obj_set_style_pad_all(joy_knob_, 0, 0);
    lv_obj_center(joy_knob_);
    lv_obj_clear_flag(joy_knob_, LV_OBJ_FLAG_CLICKABLE);

    /* 摇杆方向文字（居中实时显示 "FWD" / "STOP" 等） */
    joy_dir_lbl_ = lv_label_create(scr);
    lv_label_set_text(joy_dir_lbl_, "STOP");
    lv_obj_set_style_text_color(joy_dir_lbl_, lv_color_hex(0x5080B0), 0);
    lv_obj_set_pos(joy_dir_lbl_,
        JOY_X + JOY_DIA/2 - 20,
        JOY_Y + JOY_DIA + 4);

    /* 注册摇杆事件 */
    lv_obj_add_event_cb(joy_base_, JoystickEventCb, LV_EVENT_PRESSING,   this);
    lv_obj_add_event_cb(joy_base_, JoystickEventCb, LV_EVENT_RELEASED,   this);
    lv_obj_add_event_cb(joy_base_, JoystickEventCb, LV_EVENT_PRESS_LOST, this);

    /* ====================================================================
     * 4. 右侧控制面板背景
     * ==================================================================== */
    lv_obj_t* right_panel = lv_obj_create(scr);
    lv_obj_set_pos(right_panel, CTRL_X, TOP_BAR_H);
    lv_obj_set_size(right_panel, CTRL_W, MAIN_H);
    lv_obj_set_style_bg_color(right_panel, CLR_BG_PANEL, 0);
    lv_obj_set_style_bg_opa(right_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(right_panel, 0, 0);
    lv_obj_set_style_radius(right_panel, 0, 0);
    lv_obj_set_style_pad_all(right_panel, 0, 0);
    lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 列标题 ---- */
    const char* col_titles[] = { "TILT", "RECL", "LEGS" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, col_titles[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x6090C0), 0);
        lv_obj_set_pos(lbl, CTRL_X + i * CBTN_W_ACT + CBTN_W_ACT/2 - 16,
                             CBTN_Y0 - 17);
    }

    /* ---- UP 行 ---- */
    MakeCtrlBtn(scr, LV_SYMBOL_UP " TILT",
                CTRL_X + 0*CBTN_W_ACT + 1, CBTN_Y0, CBTN_W_ACT - 2, CBTN_H_ACT,
                WCA_TILT_UP, true);
    MakeCtrlBtn(scr, LV_SYMBOL_UP " RECL",
                CTRL_X + 1*CBTN_W_ACT + 1, CBTN_Y0, CBTN_W_ACT - 2, CBTN_H_ACT,
                WCA_RECL_UP, true);
    MakeCtrlBtn(scr, LV_SYMBOL_UP " LEGS",
                CTRL_X + 2*CBTN_W_ACT + 1, CBTN_Y0, CBTN_W_ACT - 2, CBTN_H_ACT,
                WCA_LEGS_UP, true);

    /* ---- DOWN 行 ---- */
    MakeCtrlBtn(scr, LV_SYMBOL_DOWN " TILT",
                CTRL_X + 0*CBTN_W_ACT + 1, CBTN_Y1, CBTN_W_ACT - 2, CBTN_H_ACT,
                WCA_TILT_DOWN, true);
    MakeCtrlBtn(scr, LV_SYMBOL_DOWN " RECL",
                CTRL_X + 1*CBTN_W_ACT + 1, CBTN_Y1, CBTN_W_ACT - 2, CBTN_H_ACT,
                WCA_RECL_DOWN, true);
    MakeCtrlBtn(scr, LV_SYMBOL_DOWN " LEGS",
                CTRL_X + 2*CBTN_W_ACT + 1, CBTN_Y1, CBTN_W_ACT - 2, CBTN_H_ACT,
                WCA_LEGS_DOWN, true);

    /* ---- ACT STOP (全宽) ---- */
    lv_obj_t* stop_btn = MakeCtrlBtn(scr, LV_SYMBOL_STOP " ACT STOP",
                CTRL_X + 1, CBTN_Y2, CTRL_W - 2, CBTN_H_STD,
                WCA_ACT_STOP, false);
    lv_obj_set_style_bg_color(stop_btn, CLR_BTN_STOP, 0);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(0x9E2A2A), LV_STATE_PRESSED);

    /* ---- 模式按钮 ---- */
    lv_obj_t* drv_btn = MakeCtrlBtn(scr, LV_SYMBOL_DRIVE " DRIVE",
                CTRL_X + 1, CBTN_Y3, CTRL_W/2 - 2, CBTN_H_STD,
                WCA_MODE_DRIVE, false);
    lv_obj_set_style_bg_color(drv_btn, CLR_BTN_DRIV, 0);
    lv_obj_set_style_bg_color(drv_btn, lv_color_hex(0x2E8A2E), LV_STATE_PRESSED);

    lv_obj_t* seat_btn = MakeCtrlBtn(scr, LV_SYMBOL_SETTINGS " SEAT",
                CTRL_X + CTRL_W/2 + 1, CBTN_Y3, CTRL_W/2 - 2, CBTN_H_STD,
                WCA_MODE_SEAT, false);
    lv_obj_set_style_bg_color(seat_btn, CLR_BTN_SEAT, 0);
    lv_obj_set_style_bg_color(seat_btn, lv_color_hex(0x2A2A8A), LV_STATE_PRESSED);

    /* ---- 速度调节行 ---- */
    MakeCtrlBtn(scr, LV_SYMBOL_MINUS,
                CTRL_X + 1, CBTN_Y4, 50, CBTN_H_STD - 8,
                WCA_SPD_DOWN, false);

    spd_label_ = lv_label_create(scr);
    lv_obj_set_pos(spd_label_, CTRL_X + 54, CBTN_Y4 + 6);
    lv_label_set_text_fmt(spd_label_, "SPD %d%%", WheelchairGetSpeedPct());
    lv_obj_set_style_text_color(spd_label_, CLR_TEXT, 0);

    MakeCtrlBtn(scr, LV_SYMBOL_PLUS,
                CTRL_X + CTRL_W - 51, CBTN_Y4, 50, CBTN_H_STD - 8,
                WCA_SPD_UP, false);

    /* ====================================================================
     * 5. IMU 底部条
     * ==================================================================== */
    lv_obj_t* imu_bg = lv_obj_create(scr);
    lv_obj_set_pos(imu_bg, 0, IMU_BAR_Y);
    lv_obj_set_size(imu_bg, LV_HOR_RES, IMU_BAR_H);
    lv_obj_set_style_bg_color(imu_bg, lv_color_hex(0x050D1A), 0);
    lv_obj_set_style_border_width(imu_bg, 0, 0);
    lv_obj_set_style_radius(imu_bg, 0, 0);
    lv_obj_set_style_pad_all(imu_bg, 0, 0);
    lv_obj_clear_flag(imu_bg, LV_OBJ_FLAG_SCROLLABLE);

    imu_label_ = lv_label_create(scr);
    lv_label_set_text(imu_label_, "Yaw:  0.0  P:  0.0  R:  0.0  [INIT]");
    lv_obj_set_style_text_color(imu_label_, CLR_IMU_TEXT, 0);
    lv_obj_set_pos(imu_label_, 6, IMU_BAR_Y + 6);
    lv_obj_move_foreground(imu_label_);

    /* ====================================================================
     * 6. 把顶栏/状态栏/低电量弹窗提到最前面（覆盖控制面板）
     * ==================================================================== */
    if (top_bar_)           lv_obj_move_foreground(top_bar_);
    if (status_bar_)        lv_obj_move_foreground(status_bar_);
    if (bottom_bar_)        lv_obj_move_foreground(bottom_bar_);

    /* ====================================================================
     * 7. 告警横幅（安全状态异常时浮于顶栏下方，初始隐藏）
     * ==================================================================== */
    alert_banner_ = lv_obj_create(scr);
    lv_obj_set_pos(alert_banner_, 0, TOP_BAR_H);
    lv_obj_set_size(alert_banner_, LV_HOR_RES, 38);
    lv_obj_set_style_bg_color(alert_banner_, lv_color_hex(0xCC0000), 0);
    lv_obj_set_style_bg_opa(alert_banner_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(alert_banner_, 0, 0);
    lv_obj_set_style_radius(alert_banner_, 0, 0);
    lv_obj_set_style_pad_all(alert_banner_, 0, 0);
    lv_obj_clear_flag(alert_banner_, LV_OBJ_FLAG_SCROLLABLE);

    alert_label_ = lv_label_create(alert_banner_);
    lv_label_set_text(alert_label_, LV_SYMBOL_WARNING " EMERGENCY STOP! " LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(alert_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(alert_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(alert_banner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(alert_banner_);

    /* ====================================================================
     * 8. RNetState ERROR 全屏覆盖（CAN 总线故障提示，初始隐藏）
     * ==================================================================== */
    rnet_err_overlay_ = lv_obj_create(scr);
    lv_obj_set_pos(rnet_err_overlay_, 0, 0);
    lv_obj_set_size(rnet_err_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(rnet_err_overlay_, lv_color_hex(0x7A0000), 0);
    lv_obj_set_style_bg_opa(rnet_err_overlay_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(rnet_err_overlay_, 0, 0);
    lv_obj_set_style_radius(rnet_err_overlay_, 0, 0);
    lv_obj_clear_flag(rnet_err_overlay_, LV_OBJ_FLAG_SCROLLABLE);

    rnet_err_label_ = lv_label_create(rnet_err_overlay_);
    lv_label_set_text(rnet_err_label_,
                      LV_SYMBOL_WARNING "\n\n"
                      "R-Net ERROR\n"
                      "CAN Bus Failure\n\n"
                      LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(rnet_err_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(rnet_err_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(rnet_err_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(rnet_err_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(rnet_err_overlay_);

    /* ====================================================================
     * 9. 5 Hz 定时刷新 IMU 标签
     * ==================================================================== */
    imu_timer_ = lv_timer_create(ImuTimerCb, 200, this);
}

/* ================================================================
 * JoystickEventCb —— 摇杆触摸处理
 * ================================================================ */
void CustomLcdDisplay::JoystickEventCb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
    if (!self) return;

    /* 松开 / 失去焦点 → 旋钮回中，停止 */
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_align(self->joy_knob_, LV_ALIGN_CENTER, 0, 0);
        if (self->joy_dir_lbl_) lv_label_set_text(self->joy_dir_lbl_, "STOP");
        WheelchairDirectStop();
        return;
    }
    if (code != LV_EVENT_PRESSING) return;

    /* 获取触点屏幕坐标 */
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    /* 摇杆底座中心（屏幕坐标） */
    lv_area_t area;
    lv_obj_get_coords(self->joy_base_, &area);
    int cx = (area.x1 + area.x2) / 2;
    int cy = (area.y1 + area.y2) / 2;

    float dx = (float)(pt.x - cx);
    float dy = (float)(pt.y - cy);
    float dist = sqrtf(dx * dx + dy * dy);

    /* 限制旋钮在底座圆内 */
    int max_r = JOY_DIA / 2 - JOY_KNOB_D / 2 - 4;
    if (dist > max_r) {
        dx = dx * max_r / dist;
        dy = dy * max_r / dist;
        dist = (float)max_r;
    }
    lv_obj_align(self->joy_knob_, LV_ALIGN_CENTER, (int)dx, (int)dy);

    /* 死区 15% → 停止 */
    float dead = max_r * 0.15f;
    if (dist < dead) {
        if (self->joy_dir_lbl_) lv_label_set_text(self->joy_dir_lbl_, "STOP");
        WheelchairDirectStop();
        return;
    }

    /* 映射到 -127..127（dy 负 = 向上 = 前进） */
    int8_t speed = (int8_t)(-dy * 127.0f / max_r);
    int8_t turn  = (int8_t)( dx * 127.0f / max_r);

    /* 方向文字（8 方向） */
    if (self->joy_dir_lbl_) {
        float angle = atan2f(dy, dx) * 180.0f / 3.14159f;
        const char* dir;
        if      (angle >= -22.5f  && angle <  22.5f)  dir = LV_SYMBOL_RIGHT " RIGHT";
        else if (angle >=  22.5f  && angle <  67.5f)  dir = LV_SYMBOL_DOWN  " BWD-R";
        else if (angle >=  67.5f  && angle < 112.5f)  dir = LV_SYMBOL_DOWN  " BWD";
        else if (angle >= 112.5f  && angle < 157.5f)  dir = LV_SYMBOL_DOWN  " BWD-L";
        else if (angle >= 157.5f  || angle < -157.5f) dir = LV_SYMBOL_LEFT  " LEFT";
        else if (angle >= -157.5f && angle < -112.5f) dir = LV_SYMBOL_UP    " FWD-L";
        else if (angle >= -112.5f && angle <  -67.5f) dir = LV_SYMBOL_UP    " FWD";
        else                                            dir = LV_SYMBOL_UP    " FWD-R";
        lv_label_set_text(self->joy_dir_lbl_, dir);
    }

    WheelchairDirectDrive(speed, turn);
}

/* ================================================================
 * ControlBtnCb —— 普通控制按钮（电推杆 / 模式 / 速度）
 * ================================================================ */
void CustomLcdDisplay::ControlBtnCb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    WcAction action = (WcAction)(intptr_t)lv_event_get_user_data(e);

    bool pressing  = (code == LV_EVENT_PRESSED   || code == LV_EVENT_PRESSING);
    bool releasing = (code == LV_EVENT_RELEASED  || code == LV_EVENT_PRESS_LOST);
    bool clicked   = (code == LV_EVENT_CLICKED);

    switch (action) {
    /* ---- 电推杆（长按保持）---- */
    case WCA_TILT_UP:
        if (pressing)  WheelchairDirectActuator(RNET_MOTOR_TILT, true);
        if (releasing) WheelchairDirectActuatorStop();
        break;
    case WCA_TILT_DOWN:
        if (pressing)  WheelchairDirectActuator(RNET_MOTOR_TILT, false);
        if (releasing) WheelchairDirectActuatorStop();
        break;
    case WCA_RECL_UP:
        if (pressing)  WheelchairDirectActuator(RNET_MOTOR_RECLINE, true);
        if (releasing) WheelchairDirectActuatorStop();
        break;
    case WCA_RECL_DOWN:
        if (pressing)  WheelchairDirectActuator(RNET_MOTOR_RECLINE, false);
        if (releasing) WheelchairDirectActuatorStop();
        break;
    case WCA_LEGS_UP:
        if (pressing)  WheelchairDirectActuator(RNET_MOTOR_LEGS, true);
        if (releasing) WheelchairDirectActuatorStop();
        break;
    case WCA_LEGS_DOWN:
        if (pressing)  WheelchairDirectActuator(RNET_MOTOR_LEGS, false);
        if (releasing) WheelchairDirectActuatorStop();
        break;

    /* ---- ACT STOP（点击）---- */
    case WCA_ACT_STOP:
        if (clicked || pressing) WheelchairDirectActuatorStop();
        break;

    /* ---- 驾驶模式 / 座椅模式（点击）---- */
    case WCA_MODE_DRIVE:
        if (clicked) {
            ESP_LOGI("WC_UI", "切换到驾驶模式");
            /* R-Net 模式切换预留：调用 setProfile(1) 或具体 CAN 帧 */
        }
        break;
    case WCA_MODE_SEAT:
        if (clicked) {
            ESP_LOGI("WC_UI", "切换到座椅模式");
            /* R-Net 模式切换预留：调用 setProfile(2) 或具体 CAN 帧 */
        }
        break;

    /* ---- 速度调节（点击）---- */
    case WCA_SPD_UP:
        if (clicked) {
            int8_t pct = WheelchairGetSpeedPct() + 10;
            if (pct > 100) pct = 100;
            WheelchairSetSpeedPct(pct);
        }
        break;
    case WCA_SPD_DOWN:
        if (clicked) {
            int8_t pct = WheelchairGetSpeedPct() - 10;
            if (pct < 10) pct = 10;
            WheelchairSetSpeedPct(pct);
        }
        break;
    }
}

/* ================================================================
 * ImuTimerCb —— 5Hz 刷新 IMU 条 + 速度标签
 * ================================================================ */
void CustomLcdDisplay::ImuTimerCb(lv_timer_t* timer)
{
    auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
    if (!self) return;

    /* IMU 姿态 */
    float pitch = 0.0f, roll = 0.0f;
    QMI8658GetAttitude(&pitch, &roll);
    float yaw = HWT101GetYaw();

    /* 安全状态 */
    static const char* state_str[] = { "SAFE", "STOP", "ERR ", "EMRG" };
    SafetyState ss = WheelchairGetSafetyState();
    const char* state = (ss >= SAFETY_NORMAL && ss <= SAFETY_EMERGENCY)
                        ? state_str[ss] : "?   ";

    if (self->imu_label_) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "Y:%+6.1f\xc2\xb0  P:%+5.1f\xc2\xb0  R:%+5.1f\xc2\xb0  [%s]",
                 yaw, pitch, roll, state);
        lv_label_set_text(self->imu_label_, buf);

        /* 急停时变红 */
        lv_color_t imu_color = (ss == SAFETY_EMERGENCY || ss == SAFETY_ERROR)
                               ? lv_color_hex(0xFF4444) : CLR_IMU_TEXT;
        lv_obj_set_style_text_color(self->imu_label_, imu_color, 0);
    }

    /* 速度标签 */
    if (self->spd_label_) {
        lv_label_set_text_fmt(self->spd_label_, "SPD %d%%", WheelchairGetSpeedPct());
    }

    /* 告警横幅：EMERGENCY 或 ERROR 时显示 */
    if (self->alert_banner_ && self->alert_label_) {
        if (ss == SAFETY_EMERGENCY) {
            lv_label_set_text(self->alert_label_,
                              LV_SYMBOL_WARNING " EMERGENCY STOP! 急停 " LV_SYMBOL_WARNING);
            lv_obj_clear_flag(self->alert_banner_, LV_OBJ_FLAG_HIDDEN);
        } else if (ss == SAFETY_ERROR) {
            lv_label_set_text(self->alert_label_,
                              LV_SYMBOL_WARNING " SAFETY ERROR " LV_SYMBOL_WARNING);
            lv_obj_clear_flag(self->alert_banner_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(self->alert_banner_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* RNet ERROR 全屏覆盖：CAN 总线故障时显示 */
    if (self->rnet_err_overlay_) {
        if (WheelchairIsRNetError()) {
            lv_obj_clear_flag(self->rnet_err_overlay_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(self->rnet_err_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}