#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "led.h"
#include "tim.h"
#include "MQTT.h"
#include "dht22.h"
#include "uart_cmd.h"
#include "display.h"


void app_main(void)
{
    float temperature = 0.0f;      
    float humidity    = 0.0f;     

    led_init();             //LED 初始化
    relay_init();           //继电器初始化      
    dht22_init();           //DHT22 初始化      
    tim_init();             //定时器初始化      
    tim_start(TIM_0);       //启动定时器0              
    uart_cmd_init();        //串口指令接收(语音模块), 放在网络初始化之前, 上电即可用
    display_init();         //TFT 屏幕显示(网络/温湿度/继电器)
    net_init();             //网络初始化                  

    while (1) 
    {
        printf("System Init\n");

        if (dht22_read(&temperature, &humidity)) 
        {
            g_temperature = temperature;
            g_humidity    = humidity;
            printf("[DHT22] temp = %.1f C, humi = %.1f %%\n", temperature, humidity);
        } else 
        {
            printf("[DHT22] read failed, retry next loop\n");
        }
        gpio_toggle(GPIO_NUM_1);  

        vTaskDelay(pdMS_TO_TICKS(5000));
        

/*
    gpio_set_level(GPIO_NUM_4, 0);   
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(GPIO_NUM_4, 1);   
    vTaskDelay(pdMS_TO_TICKS(1000));
*/

    }
}
