#include "display.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "st7735.h"
#include "wifi.h"
#include "MQTT.h"

#define DISPLAY_TASK_STACK      4096    // 显示任务栈大小, 字节
#define DISPLAY_TASK_PRIO       3       // 任务优先级, 高于默认的 1, 低于 WiFi/MQTT 的 5
#define DISPLAY_REFRESH_MS      1000   // 刷新间隔, 毫秒

// 每行固定 15 个字符(15*8=120 像素), 用空格补齐, 避免上一次的字残留
static void show_line(int y, const char *text, uint16_t color)
{
    char padded[18];

    snprintf(padded, sizeof(padded), "%-15s", text);
    st7735_draw_string(4, (uint16_t)y, padded, color, TFT_BLACK);
}

//显示任务
static void display_task(void *arg)
{
    (void)arg;

    char text[32];

    while (1) {
        bool wifi_ok = wifi_is_connected();
        bool mqtt_ok = mqtt_is_connected();

        show_line(22, "MY Smart Home", TFT_CYAN);

        snprintf(text, sizeof(text), "WiFi: %s", wifi_ok ? "OK" : "--");
        show_line(40, text, wifi_ok ? TFT_GREEN : TFT_RED);

        snprintf(text, sizeof(text), "MQTT: %s", mqtt_ok ? "OK" : "--");
        show_line(58, text, mqtt_ok ? TFT_GREEN : TFT_RED);

        snprintf(text, sizeof(text), "T:%.1fC H:%.1f%%", (double)g_temperature, (double)g_humidity);
        show_line(78, text, TFT_YELLOW);

        // 4 路继电器: 两列两行, 吸合绿色, 断开灰色
        for (int i = 0; i < RELAY_COUNT; i++) {
            bool on = relay_get(i);
            char cell[8];

            snprintf(text, sizeof(text), "R%d:%s", i + 1, on ? "ON" : "OFF");
            snprintf(cell, sizeof(cell), "%-6s", text);
            st7735_draw_string((uint16_t)(4 + (i % 2) * 60),
                               (uint16_t)(100 + (i / 2) * 20),
                               cell,
                               on ? TFT_GREEN : TFT_GRAY,
                               TFT_BLACK);
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS));
    }
}

//初始化
void display_init(void)
{
    if (!st7735_init()) {
        printf("[TFT] init failed, display disabled\n");
        return;
    }

    // 顶部标题栏
    st7735_fill_rect(0, 0, TFT_WIDTH, 18, TFT_DARKGRAY);
    st7735_draw_string(36, 1, "Display", TFT_WHITE, TFT_DARKGRAY);   // 7 字符 x 8 = 56, (128-56)/2 = 36 居中

    // 底部提示
    st7735_fill_rect(0, TFT_HEIGHT - 18, TFT_WIDTH, 18, TFT_DARKGRAY);
    st7735_draw_string(4, TFT_HEIGHT - 17, "System Running ", TFT_WHITE, TFT_DARKGRAY);

    xTaskCreate(display_task, "display", DISPLAY_TASK_STACK, NULL, DISPLAY_TASK_PRIO, NULL);

    printf("[TFT] display task started\n");
}
