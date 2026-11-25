#ifndef __ST7735_H_
#define __ST7735_H_

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

// ==================== 屏幕参数 ====================
#define TFT_WIDTH           128
#define TFT_HEIGHT          160

// ==================== 引脚定义(可按实际接线改) ====================
// 说明: 只用 MOSI(写屏幕), 不需要 MISO
#define TFT_SCK_GPIO        GPIO_NUM_12     // SCL/SCK
#define TFT_MOSI_GPIO       GPIO_NUM_11     // SDA/MOSI
#define TFT_CS_GPIO         GPIO_NUM_10     // CS
#define TFT_DC_GPIO         GPIO_NUM_9      // DC(A0)
#define TFT_RST_GPIO        GPIO_NUM_8      // RES/RST
#define TFT_BL_GPIO         GPIO_NUM_14     // BLK 背光, 不接则屏幕常亮

// ==================== RGB565 颜色 ====================
#define TFT_BLACK           0x0000
#define TFT_WHITE           0xFFFF
#define TFT_RED             0xF800
#define TFT_GREEN           0x07E0
#define TFT_BLUE            0x001F
#define TFT_YELLOW          0xFFE0
#define TFT_CYAN            0x07FF
#define TFT_ORANGE          0xFA20
#define TFT_GRAY            0x8410
#define TFT_DARKGRAY        0x4208

// 自定义颜色: 三个分量都是 0~255, 例如 TFT_RGB565(255, 128, 0) 是橙色
#define TFT_RGB565(r, g, b)     (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

// 初始化 SPI + 屏幕, 成功返回 true
bool st7735_init(void);

// 整屏填充
void st7735_fill(uint16_t color);

// 填充矩形(x,y 左上角, w,h 宽高)
void st7735_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

// 画一个点
void st7735_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

// 显示一个 8x16 字符
void st7735_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);

// 显示字符串(每个字符 8 像素宽, 16 像素高)
void st7735_draw_string(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);

// 背光开关
void st7735_backlight(bool on);

#endif
