#ifndef __TIM_H_
#define __TIM_H_

#include <stdint.h>

/* 定时器编号 */
typedef enum {
    TIM_0 = 0,      /* 定时器 0 */
    TIM_1,          /* 定时器 1 */
    TIM_2,          /* 定时器 2 */
    TIM_3,          /* 定时器 3 */
    TIM_MAX,        /* 定时器个数, 仅用于边界判断 */
} tim_id_t;

void tim_init(void);
void tim_start(tim_id_t id);
void tim_stop(tim_id_t id);


extern volatile uint8_t LED_flag;




#endif /* __TIM_H_ */
