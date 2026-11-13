#include "Headfile.h"

uint8_t tim2_tick=0;

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
	if(htim->Instance==TIM2)
	{
		tim2_tick++;
		if(tim2_tick==100)
		{
			tim2_tick=0;
		}
			
	
	}
}
