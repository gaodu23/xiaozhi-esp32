#include "qmi8658_sensor.h"
#include "i2c_device.h"

#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <atomic>

#define TAG "QMI8658"

/* ===== QMI8658 寄存器定义 ===== */
#define QMI_ADDR        0x6B    // SA0 接 VCC
#define QMI_WHO_AM_I    0x00    // Expected: 0x05
#define QMI_CTRL1       0x02    // SBI_2C (I2C 模式，地址自增)
#define QMI_CTRL2       0x03    // 加速度计配置 (±4g, 500Hz ODR)
#define QMI_CTRL3       0x04    // 陀螺仪配置 (±512dps, 500Hz ODR)
#define QMI_CTRL7       0x08    // 使能控制 bit0=aEN, bit1=gEN
#define QMI_AX_L        0x35    // 输出数据起始地址 (Accel X 低字节)

/* 比例因子 */
#define ACC_SCALE   (4.0f  / 32768.0f)   // ±4g  → 单位 g
#define GYRO_SCALE  (512.0f / 32768.0f)  // ±512dps → 单位 dps

/* 互补滤波参数 */
#define CF_ALPHA    0.96f   // 陀螺仪权重
#define TASK_HZ     100     // 任务频率 (100Hz)
#define DT          (1.0f / TASK_HZ)

/* ===== IMU 设备类（继承 I2cDevice） ===== */
class Qmi8658 : public I2cDevice {
public:
    Qmi8658(i2c_master_bus_handle_t bus) : I2cDevice(bus, QMI_ADDR) {}

    bool Init() {
        uint8_t id = ReadReg(QMI_WHO_AM_I);
        if (id != 0x05) {
            ESP_LOGE(TAG, "WHO_AM_I=0x%02X，期望 0x05，检查地址/连线", id);
            return false;
        }
        WriteReg(QMI_CTRL1, 0x40);   // 地址自增
        WriteReg(QMI_CTRL2, 0x16);   // AcclFSR=±4g, ODR=500Hz
        WriteReg(QMI_CTRL3, 0x56);   // GyroFSR=±512dps, ODR=500Hz
        WriteReg(QMI_CTRL7, 0x03);   // 同时使能加速度计 + 陀螺仪
        ESP_LOGI(TAG, "QMI8658 初始化成功 (addr=0x%02X)", QMI_ADDR);
        return true;
    }

    void ReadRaw(int16_t* ax, int16_t* ay, int16_t* az,
                 int16_t* gx, int16_t* gy, int16_t* gz)
    {
        uint8_t buf[12];
        ReadRegs(QMI_AX_L, buf, 12);
        *ax = (int16_t)(((uint16_t)buf[1]  << 8) | buf[0]);
        *ay = (int16_t)(((uint16_t)buf[3]  << 8) | buf[2]);
        *az = (int16_t)(((uint16_t)buf[5]  << 8) | buf[4]);
        *gx = (int16_t)(((uint16_t)buf[7]  << 8) | buf[6]);
        *gy = (int16_t)(((uint16_t)buf[9]  << 8) | buf[8]);
        *gz = (int16_t)(((uint16_t)buf[11] << 8) | buf[10]);
    }
};

/* ===== 姿态角共享状态 ===== */
static std::atomic<float> s_pitch{0.0f};
static std::atomic<float> s_roll{0.0f};
static i2c_master_bus_handle_t s_i2c_bus = nullptr;

/* ===== QMI8658 采集任务 ===== */
static void qmi8658_task(void* /*arg*/)
{
    Qmi8658 imu(s_i2c_bus);
    if (!imu.Init()) {
        ESP_LOGE(TAG, "任务退出：初始化失败");
        vTaskDelete(nullptr);
        return;
    }

    float pitch_cf = 0.0f;
    float roll_cf  = 0.0f;

    while (true) {
        int16_t ax_r, ay_r, az_r, gx_r, gy_r, gz_r;
        imu.ReadRaw(&ax_r, &ay_r, &az_r, &gx_r, &gy_r, &gz_r);

        /* 转换为物理单位 */
        float ax = ax_r * ACC_SCALE;
        float ay = ay_r * ACC_SCALE;
        float az = az_r * ACC_SCALE;
        float gx = gx_r * GYRO_SCALE;   // °/s
        float gy = gy_r * GYRO_SCALE;

        /* 加速度计估算（应对静态时的倾角） */
        float acc_norm = sqrtf(ax*ax + ay*ay + az*az);
        if (acc_norm > 0.3f && acc_norm < 3.0f) {   // 滤掉剧烈震动
            float pitch_acc =  atan2f(ay, sqrtf(ax*ax + az*az)) * (180.0f / M_PI);
            float roll_acc  = -atan2f(ax, az)                   * (180.0f / M_PI);

            /* 互补滤波：CF_ALPHA 权重给陀螺仪积分 */
            pitch_cf = CF_ALPHA * (pitch_cf + gx * DT) + (1.0f - CF_ALPHA) * pitch_acc;
            roll_cf  = CF_ALPHA * (roll_cf  + gy * DT) + (1.0f - CF_ALPHA) * roll_acc;
        } else {
            /* 加速度异常时纯陀螺仪积分 */
            pitch_cf += gx * DT;
            roll_cf  += gy * DT;
        }

        /* 限制范围 */
        if (pitch_cf >  90.0f) pitch_cf =  90.0f;
        if (pitch_cf < -90.0f) pitch_cf = -90.0f;
        if (roll_cf  >  180.0f) roll_cf =  180.0f;
        if (roll_cf  < -180.0f) roll_cf = -180.0f;

        s_pitch.store(pitch_cf, std::memory_order_relaxed);
        s_roll.store(roll_cf,   std::memory_order_relaxed);

        vTaskDelay(pdMS_TO_TICKS(1000 / TASK_HZ));
    }
}

/* ===== 公共 API ===== */

void QMI8658Init(i2c_master_bus_handle_t i2c_bus)
{
    s_i2c_bus = i2c_bus;
    xTaskCreatePinnedToCore(qmi8658_task, "QMI8658", 3072, nullptr, 3, nullptr, 1);
    ESP_LOGI(TAG, "QMI8658 任务已创建");
}

void QMI8658GetAttitude(float* pitch, float* roll)
{
    if (pitch) *pitch = s_pitch.load(std::memory_order_relaxed);
    if (roll)  *roll  = s_roll.load(std::memory_order_relaxed);
}
