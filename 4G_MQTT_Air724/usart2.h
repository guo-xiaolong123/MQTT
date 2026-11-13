#ifndef __USART2_H
#define __USART2_H

#include "Headfile.h"

/* ===================================================================
 * 串口2 : 与4G模块(EC800 / Air780 / SIM800等)通信, 波特率115200
 * 串口1 : 调试日志输出(USART1_Printf), 波特率115200
 * -------------------------------------------------------------------
 * 本层只负责"收发原始字节", 不做AT解析;
 * AT指令的解析/拼接/上传逻辑全部在 myAT.c 中实现。
 * =================================================================== */

/* 单帧最大接收长度(模组一次空闲帧可能包含 echo + 多行回复)
 * 若模组回复较长(如MQTT下发大JSON), 可适当加大 */
#define USART2_RX_BUFF_SIZE     512U

/* 开启串口2: 使能 NVIC + DMA + 空闲中断接收 */
void USART2_Start(void);

/* 兼容旧接口, 等价于 AT_Task() */
void USART2_Task(void);

/* 向模组发送原始数据 / 字符串 */
void USART2_SendData(const uint8_t *data, uint16_t size);
void USART2_SendString(const char *str);

/* 向模组发送格式化数据(仅调试用, 正式AT指令用 USART2_SendString) */
void USART2_Printf(const char *format, ...);

/* 读取"一帧"接收数据(读走即清除), 返回实际拷贝字节数, 0表示无新数据
 * 注意: 一帧必须一次读完, 传入的max要足够大 */
uint16_t USART2_ReadChunk(uint8_t *dst, uint16_t max);

/* 丢弃尚未读取的接收数据 */
void USART2_Flush(void);

/* 供其它模块直接查看的接收缓存(调试用) */
extern uint8_t rx_buff[USART2_RX_BUFF_SIZE];
extern uint8_t rx_copy[USART2_RX_BUFF_SIZE];
extern uint16_t len;                /* 最近一帧数据长度 */
extern volatile uint8_t  rx_flag;   /* 收到新数据标志 */
extern volatile uint16_t rx_cnt;    /* 接收帧计数器 */

#endif
