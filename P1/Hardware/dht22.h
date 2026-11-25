#ifndef __DHT22_H_
#define __DHT22_H_

#include <stdbool.h>
#include "driver/gpio.h"

// ==================== DHT22 连接 ====================
// 数据脚接 GPIO42
#define DHT22_GPIO      GPIO_NUM_42

// 初始化: 配置 GPIO 并等待传感器上电稳定(约 1 秒)
bool dht22_init(void);

// ==================== 读一次温湿度 ====================
//   temperature : 输出, 单位 ℃, 范围 -40.0 ~ 80.0
//   humidity    : 输出, 单位 %,  范围 0.0 ~ 100.0
//
// 返回值:
//   true  = 读取成功, 两个参数已被填入有效值
//   false = 失败(总线无响应 / 校验错), 两个参数保持原值不变
//
// 注意: DHT22 两次读取之间至少要间隔 2 秒, 不要放在快循环里调用。
//
// 用法:
//     float t = 0, h = 0;
//     if (dht22_read(&t, &h)) {
//         printf("温度 %.1f, 湿度 %.1f\n", t, h);
//     }
bool dht22_read(float *temperature, float *humidity);

#endif // __DHT22_H_
