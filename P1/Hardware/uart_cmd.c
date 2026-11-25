#include "uart_cmd.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "MQTT.h"

#define UART_CMD_BUF_SIZE   512     // RX 环形缓冲大小
#define CMD_TASK_STACK      3072
#define CMD_TASK_PRIO       4
#define CMD_LINE_MAX        64      // 串口指令最大长度

static QueueHandle_t s_uart_queue = NULL;   // UART 事件队列句柄

//指令拼接缓冲: 因为模块可能不带换行, 也可能一条被拆成几段到达
static char   s_rx_buf[CMD_LINE_MAX];
static size_t s_rx_len = 0;

// 需要识别的指令: hello + 8 条继电器指令
static const char *const s_cmd_tokens[] = {
    "hello",
    "sw1on",  "sw1off",
    "sw2on",  "sw2off",
    "sw3on",  "sw3off",
    "sw4on",  "sw4off",
};
#define CMD_TOKEN_NUM   (sizeof(s_cmd_tokens) / sizeof(s_cmd_tokens[0]))

// ======================================================================
// 执行一条指令: swN + on/off
// ====================================================================
static void uart_cmd_dispatch(const char *token)
{
    // 特殊指令 hello: 打印 ok
    if (strcmp(token, "hello") == 0) {
        printf("[UART] hello -> ok\n");
        return;
    }

    int relay = token[2] - '1';                 // '1'..'4' -> 0..3
    bool on = (strcmp(token + 3, "on") == 0);

    if (relay < 0 || relay >= RELAY_COUNT) {
        return;
    }

    relay_set(relay, on);                       // 复用 MQTT 里的继电器状态
    printf("[UART] %s -> relay%d %s\n", token, relay + 1, on ? "ON" : "OFF");
}


// 把收到的字节喂进缓冲, 并从中找出完整指令
//   1. 逐字节追加
//   2. 每加一个字节就在缓冲里找有没有完整的指令
//   3. 找到就执行并从缓冲里删掉, 继续找(处理粘包)
static void uart_cmd_feed(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        char c = (char)data[i];

        // 换行/回车当作分隔符: 清空缓冲, 防止垃圾字符堆积
        if (c == '\r' || c == '\n') {
            s_rx_len = 0;
            continue;
        }

        if (s_rx_len < sizeof(s_rx_buf) - 1) {
            s_rx_buf[s_rx_len++] = c;
        } else {
            // 缓冲满了还没匹配到, 丢掉最旧的 1 个字节
            memmove(s_rx_buf, s_rx_buf + 1, sizeof(s_rx_buf) - 2);
            s_rx_len = sizeof(s_rx_buf) - 1;
            s_rx_buf[s_rx_len - 1] = c;
        }
        s_rx_buf[s_rx_len] = '\0';

        // 反复查找, 处理 "sw1onsw2on" 这种粘包
        bool found = true;
        while (found) {
            found = false;

            for (unsigned k = 0; k < CMD_TOKEN_NUM; k++) {
                char *pos = strstr(s_rx_buf, s_cmd_tokens[k]);
                if (pos == NULL) {
                    continue;
                }

                uart_cmd_dispatch(s_cmd_tokens[k]);

                // 把这条指令(连同它前面的垃圾字符)从缓冲里删掉
                size_t used = (size_t)(pos - s_rx_buf) + strlen(s_cmd_tokens[k]);
                size_t remain = s_rx_len - used;
                memmove(s_rx_buf, s_rx_buf + used, remain);
                s_rx_len = remain;
                s_rx_buf[s_rx_len] = '\0';

                found = true;
                break;
            }
        }
    }
}

//串口接收任务: 从事件队列里取出 UART_DATA 事件, 读取环形缓冲里的字节, 喂给 uart_cmd_feed()
static void uart_cmd_task(void *arg)
{
    (void)arg;

    uart_event_t event; //接收消息队列第一个参数
    uint8_t data[128];

    while (1) {
        if (xQueueReceive(s_uart_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event.type == UART_DATA) {
            int remain = (int)event.size;

            while (remain > 0) {
                int want = (remain > (int)sizeof(data)) ? (int)sizeof(data) : remain;
                int len = uart_read_bytes(UART_CMD_PORT, data, want, pdMS_TO_TICKS(20));
                if (len <= 0) {
                    break;
                }
                uart_cmd_feed(data, len);
                remain -= len;
            }
        }
        else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            // 收得太快/太慢, 清空重来, 避免数据错位
            uart_flush_input(UART_CMD_PORT);
            xQueueReset(s_uart_queue);
            s_rx_len = 0;
            printf("[UART] rx overflow, buffer cleared\n");
        }
        else if (event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR) {
            printf("[UART] frame error, check baud rate/gnd\n");
        }
    }
}

//串口初始化
void uart_cmd_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = UART_CMD_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 装驱动: 中断由驱动管理, 收满一批就往队列里丢一个 UART_DATA 事件
    ESP_ERROR_CHECK(uart_driver_install(UART_CMD_PORT,
                                        UART_CMD_BUF_SIZE * 2,  // RX 环形缓冲
                                        0,                      // TX 缓冲不用
                                        20,                     // 事件队列长度
                                        &s_uart_queue,
                                        0));

    ESP_ERROR_CHECK(uart_param_config(UART_CMD_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_CMD_PORT,
                                 UART_CMD_TX_PIN,
                                 UART_CMD_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    xTaskCreate(uart_cmd_task, "uart_cmd", CMD_TASK_STACK, NULL, CMD_TASK_PRIO, NULL);

    printf("[UART] cmd ready: uart%d, rx=io%d, %d bps\n",
           UART_CMD_PORT, UART_CMD_RX_PIN, UART_CMD_BAUD);
}
