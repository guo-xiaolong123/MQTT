#ifndef __UART_CMD_H_
#define __UART_CMD_H_

#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/uart.h"

// ==================== 串口指令接收配置 ====================
// 只做接收(RX), TX 不接设备, 波特率 9600
//
// 接线:
//   语音模块 TX  ── ESP32 GPIO18 (UART_CMD_RX_PIN)
//   语音模块 GND ── ESP32 GND
//   (语音模块用自己电源时, 必须共地)
//
// 如果实际接的不是 IO18, 改下面的 UART_CMD_RX_PIN 即可
#define UART_CMD_PORT       UART_NUM_1
#define UART_CMD_BAUD       9600
#define UART_CMD_RX_PIN     GPIO_NUM_18     // 接语音模块的 TX
#define UART_CMD_TX_PIN     GPIO_NUM_17     // 本工程不用 TX, 仅占位, 可改

// 初始化串口 + 创建解析任务
// 内部会配置 UART、安装驱动、创建 "uart_cmd" 任务,
// 收到 sw1on/sw1off ... sw4on/sw4off 时自动控制对应继电器。
void uart_cmd_init(void);

#endif // __UART_CMD_H_
