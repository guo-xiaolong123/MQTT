#include "Headfile.h"

/* ===================================================================
 * 用户代码区
 * -------------------------------------------------------------------
 * 平台下发"属性设置"后, myAT.c 会自动解析, 把值赋给 g_recv_xxx 变量,
 *
 *   方式一: 写 AT_OnRecvProperty() 回调, 一收到就立即执行动作。
 *           myAT.c 里已经有一个同名的空函数(叫"弱函数"),
 *           你在本文件里再写一个一模一样的, 编译器会自动用你的,
 *           不用改 myAT.c, 也不会报重复定义。
 *
 * =================================================================== */

void Task_Init(void)
{
	USART2_Start();				   // 开启串口二DMA配合串口空闲中断
	AT_Init();					   // AT驱动初始化(确保串口二接收已开启)
	HAL_TIM_Base_Start_IT(&htim2); // 定时器二1s中断计时
	USART1_Printf("sys_start\r\n");
}

void Task(void)
{
	/* 云平台任务: 自动连接 + 周期上传 + 接收平台下发 */
	AT_Cloud_Task();
}

/* ===================================================================
 * 平台下发属性时自动执行(弱函数回调)

 * =================================================================== */
void AT_OnRecvProperty(const char *name, const AT_Value *val)
{
	(void)val; /* 这里用全局变量, val 只是备用 */

	/* 第一类: 0/1 开关量 -> 点亮/熄灭 PC13 的 LED */
	if (strcmp(name, "sw1") == 0)
	{
		if (g_recv_sw1 != 0)
		{
			HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET); /* 低电平: LED亮 */
		}
		else
		{
			HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET); /* 高电平: LED灭 */
		}
		USART1_Printf("SW1 = %u\r\n", (unsigned)g_recv_sw1);
	}
	/* 第二类: 整数 -> 这里先打印, 你要控制电机/舵机就换成自己的函数 */
	else if (strcmp(name, "num1") == 0)
	{
		USART1_Printf("NUM1 = %ld\r\n", (long)g_recv_num1);
	}
	/* 你平台上现在的 test1 / test2 属性(整数) */
	else if (strcmp(name, "test1") == 0)
	{
		USART1_Printf("TEST1 = %ld\r\n", (long)g_recv_test1);
	}
	else if (strcmp(name, "test2") == 0)
	{
		USART1_Printf("TEST2 = %ld\r\n", (long)g_recv_test2);
	}
	/* 其它属性: 打印出来方便调试 */
	else
	{
		USART1_Printf("recv other: %s\r\n", name);
	}
}
