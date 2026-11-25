// ***********************************************************************
// 文件名   : dht22.c
// 功能     : DHT22(AM2302) 温湿度传感器驱动, 单总线时序, GPIO 模拟
// 平台     : ESP32-S3 / ESP-IDF v5.1.2
//
// 时序说明(DHT22 手册):
//   主机拉低 >= 1ms  ->  释放总线
//   传感器响应: 80us 低 + 80us 高
//   40 bit 数据: 每 bit = 50us 低 + 高电平
//                高电平 26~28us = 0, 70us = 1
//   数据格式: 湿度整数(8) 湿度小数(8) 温度整数(8) 温度小数(8) 校验(8)
//   温度最高位为符号位, 1 = 负温度
// ***********************************************************************

#include "dht22.h"

#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

// ---------------- 时序参数 ----------------
#define DHT22_START_LOW_US      1200    // 主机拉低时间, 要求 >= 1ms
#define DHT22_RELEASE_US        30      // 释放总线后等 20~40us
#define DHT22_TIMEOUT_US        200     // 等电平跳变的超时时间

// 读 40 bit 期间是否关闭中断/调度:
//   1 = 关(推荐, 读数稳定; 但会封锁约 4~5ms, WiFi 偶尔丢一两个包, 能恢复)
//   0 = 不关(可能偶发读失败, 主循环下个周期再读即可)
#define DHT22_USE_CRITICAL      1

static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

// ======================================================================
// 等待总线变成指定电平, 超时返回 false
// ====================================================================
static bool dht22_wait_level(int level, int timeout_us)
{
    int waited = 0;

    while (gpio_get_level(DHT22_GPIO) != level) {
        if (waited >= timeout_us) {
            return false;
        }
        esp_rom_delay_us(1);
        waited++;
    }
    return true;
}

// ======================================================================
// 完整读一次 40 bit 原始数据(含起始信号)
// ====================================================================
static bool dht22_read_raw(uint8_t data[5])
{
    // 1. 主机把总线拉低至少 1ms, 作为起始信号
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT22_GPIO, 0);
    esp_rom_delay_us(DHT22_START_LOW_US);

    // 2. 释放总线(改成输入), 由上拉电阻把线拉高
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);
    esp_rom_delay_us(DHT22_RELEASE_US);

    // 3. 等传感器响应: 80us 低 + 80us 高
    if (!dht22_wait_level(0, DHT22_TIMEOUT_US)) {       // 等它把线拉低
        return false;
    }
    if (!dht22_wait_level(1, DHT22_TIMEOUT_US)) {       // 80us 低电平结束
        return false;
    }
    if (!dht22_wait_level(0, DHT22_TIMEOUT_US)) {       // 80us 高电平结束, 开始第 1 bit
        return false;
    }

    // 4. 逐位读取 40 bit
    for (int i = 0; i < 5; i++) {
        data[i] = 0;

        for (int b = 0; b < 8; b++) {
            // 每 bit 先有 50us 低电平, 等它结束(上升沿)
            if (!dht22_wait_level(1, DHT22_TIMEOUT_US)) {
                return false;
            }

            // 上升沿后 40us 采样: 0 已经变低, 1 仍然是高
            esp_rom_delay_us(40);

            data[i] <<= 1;
            if (gpio_get_level(DHT22_GPIO)) {           // 还是高 = 该位是 1
                data[i] |= 1;
                if (!dht22_wait_level(0, DHT22_TIMEOUT_US)) {
                    return false;
                }
            }
        }
    }

    return true;
}

// ======================================================================
// 初始化: 配置 GPIO, 等传感器稳定
// ====================================================================
bool dht22_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ull << DHT22_GPIO,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    if (gpio_config(&io_cfg) != ESP_OK) {
        printf("[DHT22] gpio_config failed\n");
        return false;
    }
    gpio_set_level(DHT22_GPIO, 1);

    // 上电后 DHT22 需要约 1 秒稳定, 之后再读才可靠
    vTaskDelay(pdMS_TO_TICKS(1000));

    printf("[DHT22] init ok, data pin = %d\n", (int)DHT22_GPIO);
    return true;
}

// ======================================================================
// 读一次温湿度
// ====================================================================
bool dht22_read(float *temperature, float *humidity)
{
    if (temperature == NULL || humidity == NULL) {
        return false;
    }

    uint8_t data[5] = {0};
    bool ok;

    // 时序敏感: 读这 40 bit 期间不希望被任务切换打断
#if DHT22_USE_CRITICAL
    taskENTER_CRITICAL(&s_dht_mux);
#endif

    ok = dht22_read_raw(data);

#if DHT22_USE_CRITICAL
    taskEXIT_CRITICAL(&s_dht_mux);
#endif

    if (!ok) {
        return false;
    }

    // 校验: 前 4 字节相加的低 8 位, 应等于第 5 字节
    uint8_t sum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (sum != data[4]) {
        return false;
    }

    uint16_t raw_humi = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_temp = ((uint16_t)data[2] << 8) | data[3];

    // 湿度: 原始值 x10
    float h = (float)raw_humi / 10.0f;

    // 温度: 最高位是符号位, 1 表示零下
    float t = (float)(raw_temp & 0x7FFF) / 10.0f;
    if (raw_temp & 0x8000) {
        t = -t;
    }

    *temperature = t;
    *humidity    = h;

    return true;
}
