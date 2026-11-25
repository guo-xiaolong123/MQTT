#include "st7735.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"

#include "tft_font.h"       // asc[] 8x16 ASCII 字库

#define TFT_SPI_HOST        SPI2_HOST
#define TFT_SPI_HZ          (20 * 1000 * 1000)      // 20MHz, 屏幕花屏可降到 10MHz

static spi_device_handle_t s_tft_spi = NULL;

// 发一段字节: is_data=true 时 DC 拉高(数据), 否则拉低(命令)
static void tft_write(const uint8_t *data, size_t len, bool is_data)
{
    if (len == 0) {
        return;
    }

    gpio_set_level(TFT_DC_GPIO, is_data ? 1 : 0);

    spi_transaction_t t = {0};
    t.length    = len * 8;
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_tft_spi, &t));
}

static void tft_cmd(uint8_t cmd)
{
    tft_write(&cmd, 1, false);
}

static void tft_data(const uint8_t *data, size_t len)
{
    tft_write(data, len, true);
}

static void tft_data8(uint8_t d)
{
    tft_data(&d, 1);
}

// 设置显示窗口, 之后写入的颜色会按窗口自动换行填充
static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t b[4];

    tft_cmd(0x2A);                      // 列地址范围
    b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0;
    b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
    tft_data(b, 4);

    tft_cmd(0x2B);                      // 行地址范围
    b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0;
    b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
    tft_data(b, 4);

    tft_cmd(0x2C);                      // 开始写显存
}

// 硬件复位
static void tft_hw_reset(void)
{
    gpio_set_level(TFT_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TFT_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

// 写初始化寄存器序列(与 STM32 版一致)
static void tft_init_sequence(void)
{
    tft_cmd(0x11);                      // 退出睡眠
    vTaskDelay(pdMS_TO_TICKS(120));

    tft_cmd(0x36); tft_data8(0x00);     // 数据访问方式(MADCTL)
    tft_cmd(0x3A); tft_data8(0x05);     // 16bit 颜色

    tft_cmd(0xB1); tft_data8(0x05); tft_data8(0x3C); tft_data8(0x3C);   // 帧率
    tft_cmd(0xB2); tft_data8(0x05); tft_data8(0x3C); tft_data8(0x3C);
    tft_cmd(0xB3); tft_data8(0x05); tft_data8(0x3C); tft_data8(0x3C);
    tft_data8(0x05); tft_data8(0x3C); tft_data8(0x3C);
    tft_cmd(0xB4); tft_data8(0x03);     // 显示反转控制

    tft_cmd(0xC0); tft_data8(0x2E); tft_data8(0x06); tft_data8(0x04);   // 电源控制
    tft_cmd(0xC1); tft_data8(0xC0); tft_data8(0xC2);
    tft_cmd(0xC2); tft_data8(0x0D); tft_data8(0x0D);
    tft_cmd(0xC3); tft_data8(0x8D); tft_data8(0xEE);
    tft_cmd(0xC4); tft_data8(0x8D); tft_data8(0xEE);
    tft_cmd(0xC5); tft_data8(0x00);     // VCOM

    tft_cmd(0x36); tft_data8(0xC0);     // 方向: 纵向, 左上角为原点

    tft_cmd(0xE0);                      // 伽马
    tft_data8(0x1B); tft_data8(0x21); tft_data8(0x10); tft_data8(0x15);
    tft_data8(0x2B); tft_data8(0x25); tft_data8(0x1F); tft_data8(0x23);
    tft_data8(0x22); tft_data8(0x22); tft_data8(0x2B); tft_data8(0x37);
    tft_data8(0x00); tft_data8(0x15); tft_data8(0x02); tft_data8(0x3F);

    tft_cmd(0xE1);
    tft_data8(0x1A); tft_data8(0x20); tft_data8(0x0F); tft_data8(0x15);
    tft_data8(0x2A); tft_data8(0x25); tft_data8(0x1E); tft_data8(0x23);
    tft_data8(0x23); tft_data8(0x22); tft_data8(0x2B); tft_data8(0x37);
    tft_data8(0x00); tft_data8(0x15); tft_data8(0x02); tft_data8(0x3F);

    tft_cmd(0x2C);                      // 准备写显存
    tft_cmd(0x29);                      // 打开显示
}

// ***********************************************************************
// 初始化: 配置 GPIO + SPI 总线 + 屏幕寄存器
// ***********************************************************************
bool st7735_init(void)
{
    // 1. 控制引脚: DC / RST / BL
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ull << TFT_DC_GPIO) | (1ull << TFT_RST_GPIO) | (1ull << TFT_BL_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    gpio_set_level(TFT_BL_GPIO, 1);     // 背光先关闭, 避免上电花屏
    gpio_set_level(TFT_DC_GPIO, 0);

    // 2. SPI 总线(只用 MOSI + SCLK, 不用 MISO)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = TFT_MOSI_GPIO,
        .miso_io_num     = -1,
        .sclk_io_num     = TFT_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(TFT_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        printf("[TFT] spi_bus_initialize failed\n");
        return false;
    }

    // 3. 添加屏幕设备(CS 由驱动自动控制)
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = TFT_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = TFT_CS_GPIO,
        .queue_size     = 4,
    };
    err = spi_bus_add_device(TFT_SPI_HOST, &dev_cfg, &s_tft_spi);
    if (err != ESP_OK) {
        printf("[TFT] spi_bus_add_device failed\n");
        return false;
    }

    // 4. 复位 + 初始化序列
    tft_hw_reset();
    tft_init_sequence();
    st7735_fill(TFT_BLACK);

    gpio_set_level(TFT_BL_GPIO, 1);     // 打开背光
    printf("[TFT] st7735 init ok\n");
    return true;
}

// ***********************************************************************
// 背光开关
// ***********************************************************************
void st7735_backlight(bool on)
{
    gpio_set_level(TFT_BL_GPIO, on ? 1 : 0);
}

// ***********************************************************************
// 填充矩形
// ***********************************************************************
void st7735_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= TFT_WIDTH || y >= TFT_HEIGHT || w == 0 || h == 0) {
        return;
    }
    if ((uint32_t)x + w > TFT_WIDTH) {
        w = TFT_WIDTH - x;
    }
    if ((uint32_t)y + h > TFT_HEIGHT) {
        h = TFT_HEIGHT - y;
    }

    tft_set_window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));

    uint8_t buf[128];                   // 一次 64 个像素
    for (int i = 0; i < 64; i++) {
        buf[i * 2]     = (uint8_t)(color >> 8);
        buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }

    uint32_t total = (uint32_t)w * h;
    while (total > 0) {
        uint32_t n = (total > 64) ? 64 : total;
        tft_data(buf, n * 2);
        total -= n;
    }
}

void st7735_fill(uint16_t color)
{
    st7735_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

void st7735_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    st7735_fill_rect(x, y, 1, 1, color);
}

// ***********************************************************************
// 显示一个字符: 8x16, 一次把 256 字节发给屏幕
// ***********************************************************************
void st7735_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    if (x + 8 > TFT_WIDTH || y + 16 > TFT_HEIGHT) {
        return;
    }
    if (c < 32 || c > 126) {
        c = '?';
    }

    const unsigned char *font = &asc[(c - 32) * 16];
    uint8_t buf[8 * 16 * 2];

    for (int row = 0; row < 16; row++) {
        unsigned char bits = font[row];
        for (int col = 0; col < 8; col++) {
            uint16_t color = (bits & (0x80 >> col)) ? fg : bg;
            int idx = (row * 8 + col) * 2;
            buf[idx]     = (uint8_t)(color >> 8);
            buf[idx + 1] = (uint8_t)(color & 0xFF);
        }
    }

    tft_set_window(x, y, (uint16_t)(x + 7), (uint16_t)(y + 15));
    tft_data(buf, sizeof(buf));
}

// ***********************************************************************
// 显示字符串
// ***********************************************************************
void st7735_draw_string(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    if (s == NULL) {
        return;
    }

    uint16_t cx = x;
    uint16_t cy = y;

    while (*s != '\0') {
        if (cx + 8 > TFT_WIDTH) {       // 自动换行
            cx = x;
            cy += 16;
        }
        if (cy + 16 > TFT_HEIGHT) {
            break;
        }
        st7735_draw_char(cx, cy, *s, fg, bg);
        cx += 8;
        s++;
    }
}
