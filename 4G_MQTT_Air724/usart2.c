#include "Headfile.h"

/* ===================================================================
 * 串口2 : 4G模组通信驱动(DMA + 空闲中断  一帧接收)
 * 串口1 : 日志输出
 * -------------------------------------------------------------------
 * 注意: 原代码中 HAL_UARTEx_ReceiveToIdle_DMA(&huart1 ...) 是笔误,
 *       必须用 huart2, 否则永远收不到模组回复。
 * =================================================================== */

uint16_t len = 0;
uint8_t rx_buff[USART2_RX_BUFF_SIZE];
uint8_t rx_copy[USART2_RX_BUFF_SIZE];

volatile uint8_t  rx_flag = 0;
volatile uint16_t rx_cnt  = 0;

/* 已经读走的帧计数, 用于判断有没有新数据 */
static uint16_t rx_read_cnt = 0;

/* 重新开启一次DMA空闲接收(RxState空闲时才启动, 防止重复启动) */
static void USART2_StartReceive(void)
{
	if (huart2.RxState == HAL_UART_STATE_READY)
	{
		HAL_UARTEx_ReceiveToIdle_DMA(&huart2, rx_buff, USART2_RX_BUFF_SIZE);
	}
}

/* 开启串口2功能:
 * 1. 使能USART2全局中断(IDLE空闲中断必须要NVIC打开才能进中断!)
 * 2. 清空接收缓存
 * 3. 启动DMA+空闲接收 */
void USART2_Start(void)
{
	HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
	HAL_NVIC_EnableIRQ(USART2_IRQn);

	USART2_Flush();
	USART2_StartReceive();
}

/* 发送原始数据到模组 */
void USART2_SendData(const uint8_t *data, uint16_t size)
{
	if (data == NULL || size == 0)
	{
		return;
	}
	HAL_UART_Transmit(&huart2, (uint8_t *)data, size, 1000);
}

/* 发送字符串到模组 */
void USART2_SendString(const char *str)
{
	if (str == NULL || *str == '\0')
	{
		return;
	}
	USART2_SendData((const uint8_t *)str, (uint16_t)strlen(str));
}

/* 串口二格式化打印(调试用) */
void USART2_Printf(const char *format, ...)
{
	char buffer[128];
	va_list args;

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	USART2_SendString(buffer);
}

/* 读取一帧数据(读走即清除), 返回实际字节数 */
uint16_t USART2_ReadChunk(uint8_t *dst, uint16_t max)
{
	uint16_t n = 0;

	if (dst == NULL || max == 0)
	{
		return 0;
	}

	/* 关中断拷贝, 防止DMA空闲中断在拷贝过程中改写 rx_copy */
	__disable_irq();
	if (rx_read_cnt != rx_cnt)
	{
		rx_read_cnt = rx_cnt;
		n = len;
		if (n > max)
		{
			n = max;          /* 传进来的缓冲区太小, 多余部分丢弃 */
		}
		if (n > 0)
		{
			memcpy(dst, rx_copy, n);
		}
		if (n < max)
		{
			dst[n] = '\0';    /* 方便直接当字符串打印 */
		}
	}
	__enable_irq();

	return n;
}

/* 丢弃未读数据 */
void USART2_Flush(void)
{
	__disable_irq();
	rx_read_cnt = rx_cnt;
	__enable_irq();
}

/* 串口空闲中断回调(HAL在IDLE中断/DMA满时调用)
 * Size: 本次收到的字节数 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	if (huart->Instance == USART2)
	{
		if (Size > USART2_RX_BUFF_SIZE)
		{
			Size = USART2_RX_BUFF_SIZE;
		}

		if (Size > 0)
		{
			memcpy(rx_copy, rx_buff, Size);
			len = Size;
			rx_flag = 1;
			rx_cnt++;            /* 最后更新帧计数, 保证读端看到的是完整数据 */
		}

		/* 立即重新开启接收, 否则只能收到一帧 */
		USART2_StartReceive();
	}
}

/* 串口错误回调(溢出/帧错误后恢复接收) */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance == USART2)
	{
		USART2_StartReceive();
	}
}

/* ===================================================================
 * 串口2中断服务函数
 * 说明: 因为CubeMX里没有勾选"USART2 global interrupt", 所以放在这里定义。
 *      如果以后在CubeMX里重新生成并把该函数生成到 stm32f1xx_it.c,
 *      请把这里的函数删掉, 否则会重复定义。
 * =================================================================== */
void USART2_IRQHandler(void)
{
	HAL_UART_IRQHandler(&huart2);
}

/* 兼容旧接口: 交给AT层处理模组主动上报的数据 */
void USART2_Task(void)
{
	AT_Task();
}
