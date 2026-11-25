#include "MQTT.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "wifi.h"

#define ONENET_URI  "mqtt://" ONENET_HOST ":1883"


//继电器标识符
static const char *const s_relay_id[RELAY_COUNT] = {
    "relay1", "relay2", "relay3", "relay4",
};

//继电器 GPIO 引脚, 按顺序对应上面的标识符
static const int s_relay_gpio[RELAY_COUNT] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
};

//继电器状态: true = 吸合, false = 断开
#define RELAY_ACTIVE_LOW    1

// 继电器状态数组, true = 吸合, false = 断开
static bool s_relay_state[RELAY_COUNT] = { false };

// 温湿度全局变量
float g_temperature = 0.0f;
float g_humidity    = 0.0f;

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static bool s_report_task_started = false;

// 上报用的 JSON 缓冲
static char   s_report_buf[512];
static size_t s_report_len = 0;
static bool   s_report_first = true;
static uint32_t s_msg_id = 0;

//继电器控制
static void relay_write(int index, bool on)
{
    int level = on ? 1 : 0;

    if (RELAY_ACTIVE_LOW) {
        level = !level;
    }
    gpio_set_level((gpio_num_t)s_relay_gpio[index], level);
}

//继电器端口初始化
void relay_init(void)
{
    uint64_t pin_mask = 0;

    for (int i = 0; i < RELAY_COUNT; i++) {
        pin_mask |= (1ull << s_relay_gpio[i]);
    }

    // 推挽输出: 先把输出锁存器全部置 1(断开状态), 再切换成输出引脚,
    // 这样初始化瞬间继电器不会吸合一下
    for (int i = 0; i < RELAY_COUNT; i++) {
        gpio_set_level((gpio_num_t)s_relay_gpio[i], 1);
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_OUTPUT,      // 推挽输出: 模块 VCC 用 3.3V 时, 高电平 3.3V 就能完全关断
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    // 初始化时全部断开继电器
    for (int i = 0; i < RELAY_COUNT; i++) {
        s_relay_state[i] = false;
        relay_write(i, false);
    }

    printf("[APP] %d relays init ok\n", RELAY_COUNT);
}

//设置继电器状态
void relay_set(int index, bool on)
{
    if (index < 0 || index >= RELAY_COUNT) {
        return;
    }

    s_relay_state[index] = on;
    relay_write(index, on);
}

//获取继电器状态
bool relay_get(int index)
{
    if (index < 0 || index >= RELAY_COUNT) {
        return false;
    }
    return s_relay_state[index];
}

//发布的消息拼接JesON字符串, 拼接到缓冲区
static void report_append(const char *fmt, ...)
{
    if (s_report_len >= sizeof(s_report_buf) - 1) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_report_buf + s_report_len, sizeof(s_report_buf) - s_report_len, fmt, ap);
    va_end(ap);

    if (n > 0) {
        s_report_len += (size_t)n;
        if (s_report_len >= sizeof(s_report_buf)) {
            s_report_len = sizeof(s_report_buf) - 1;
        }
    }
}
//发布消息到物联网平台
static int mqtt_publish(const char *topic, const char *data)
{
    if (s_client == NULL || !s_connected) {
        printf("[MQTT] not connected, drop data\n");
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, data, 0, 0, 0);
}

//标识符转继电器索引
static bool relay_id_to_index(const char *id, int *index)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (strcmp(id, s_relay_id[i]) == 0) {
            *index = i;
            return true;
        }
    }
    return false;
}

//每个上报报文开头
void mqtt_report_begin(void)
{
    s_report_len = 0;
    s_report_first = true;
    report_append("{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{",
                  (unsigned long)(++s_msg_id));
}
//添加上报布尔属性
void mqtt_report_add_bool(const char *id, bool value)
{
    report_append("%s\"%s\":{\"value\":%s}", s_report_first ? "" : ",", id, value ? "true" : "false");
    s_report_first = false;
}
//添加上报整数属性
void mqtt_report_add_int(const char *id, int value)
{
    report_append("%s\"%s\":{\"value\":%d}", s_report_first ? "" : ",", id, value);
    s_report_first = false;
}
//添加上报小数属性
void mqtt_report_add_float(const char *id, float value)
{
    // 不用 %f, 避免 printf 精简版不支持浮点
    bool neg = (value < 0);
    if (neg) {
        value = -value;
    }

    int ipart = (int)value;
    int fpart = (int)((value - (float)ipart) * 100.0f + 0.5f);
    if (fpart >= 100) {
        ipart++;
        fpart -= 100;
    }

    report_append("%s\"%s\":{\"value\":%s%d.%02d}", s_report_first ? "" : ",", id, neg ? "-" : "", ipart, fpart);
    s_report_first = false;
}
//添加上报字符串属性
void mqtt_report_add_string(const char *id, const char *value)
{
    report_append("%s\"%s\":{\"value\":\"%s\"}", s_report_first ? "" : ",", id, value);
    s_report_first = false;
}
//最后完成拼接结尾并且发送上报报文到物联网平台
int mqtt_report_send(void)
{
    report_append("}}");
    printf("[MQTT] -> %s\n", s_report_buf);
    return mqtt_publish(TOPIC_PROP_POST, s_report_buf);
}

//直接单独上报布尔属性
void mqtt_upload_bool(const char *id, bool value)
{
    mqtt_report_begin();
    mqtt_report_add_bool(id, value);
    mqtt_report_send();
}
//直接单独上报整数属性
void mqtt_upload_int(const char *id, int value)
{
    mqtt_report_begin();
    mqtt_report_add_int(id, value);
    mqtt_report_send();
}
//直接单独上报小数属性
void mqtt_upload_float(const char *id, float value)
{
    mqtt_report_begin();
    mqtt_report_add_float(id, value);
    mqtt_report_send();
}

//回复云平台的属性设置请求
void mqtt_reply_set(const char *msg_id, int code, const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}", msg_id, code, msg);
    mqtt_publish(TOPIC_PROP_SET_REPLY, buf);
}

//处理云平台下发指令和消息
static void mqtt_do_action(const char *id, mqtt_value_t value)
{
    int index = 0;
    if (relay_id_to_index(id, &index) && value.type == MQTT_VAL_BOOL) //继电器
    {
        s_relay_state[index] = value.v.b;
        relay_write(index, s_relay_state[index]);
        printf("[APP] %s -> %d\n", id, s_relay_state[index]);
    }
    else if (strcmp(id, PROP_TEMP) == 0 && value.type == MQTT_VAL_FLOAT)//温度
    {
        g_temperature = (float)value.v.f;
        printf("[APP] %s -> %.2f\n", id, (double)g_temperature);
    }
    // else if (strcmp(id, PROP_HUMI) == 0 && value.type == MQTT_VAL_FLOAT) //湿度
    // {
    // g_humidity = (float)value.v.f;
    // printf("[APP] %s -> %.2f\n", id, (double)g_humidity);
    // }
    // else if (strcmp(id, "led") == 0 && value.type == MQTT_VAL_BOOL) //LED
    // {
    // printf("[APP] led -> %d\n", value.v.b);
    // //gpio_set_level(GPIO_NUM_10, value.v.b);
    // }
    // else if (strcmp(id, "speed") == 0 && value.type == MQTT_VAL_INT)//整数属性
    // {
    // printf("[APP] speed -> %d\n", value.v.i);
    // }
    // else if (strcmp(id, "target_temp") == 0 && value.type == MQTT_VAL_FLOAT) //小数属性
    // {
    // printf("[APP] target_temp -> %.2f\n", value.v.f);
    // }
    //
    // else if (strcmp(id, "mode") == 0 && value.type == MQTT_VAL_STRING) //字符串属性
    // {
    // printf("[APP] mode -> %s\n", value.v.s);
    // }

    else //未匹配属性
    {
        printf("[APP] unknown property: %s\n", id);
    }
    
}



//解析{"id":"2","version":"1.0","params":{"relay1":true,"temp":26.5}}
static void handle_property_set(const char *payload, int len)
{
    char buf[512];
    int n = (len < (int)sizeof(buf) - 1) ? len : (int)sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        printf("[MQTT] json parse error\n");
        return;
    }

    const char *msg_id = "0";
    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id_item)) {
        msg_id = id_item->valuestring;
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (cJSON_IsObject(params)) {
        for (cJSON *item = params->child; item != NULL; item = item->next) {
            mqtt_value_t value = {0};

            if (cJSON_IsBool(item)) {
                value.type = MQTT_VAL_BOOL;
                value.v.b = cJSON_IsTrue(item);
            }
            else if (cJSON_IsNumber(item)) {
                double d = item->valuedouble;
                if (d == (double)(int)d) {
                    value.type = MQTT_VAL_INT;
                    value.v.i = (int)d;
                } else {
                    value.type = MQTT_VAL_FLOAT;
                    value.v.f = d;
                }
            }
            else if (cJSON_IsString(item)) {
                value.type = MQTT_VAL_STRING;
                value.v.s = item->valuestring;
            }
            else {
                continue;
            }

            mqtt_do_action(item->string, value);
        }
    }

    mqtt_reply_set(msg_id, 200, "success");
    cJSON_Delete(root);
}

//由freeRTOS任务调用, 定时上报属性到物联网平台
static void mqtt_report_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_connected) {
            mqtt_report_begin();

            // 6 路继电器
            for (int i = 0; i < RELAY_COUNT; i++) {
                mqtt_report_add_bool(s_relay_id[i], s_relay_state[i]);
            }

            // 温度 / 湿度
            mqtt_report_add_float(PROP_TEMP, g_temperature);
            mqtt_report_add_float(PROP_HUMI, g_humidity);

            mqtt_report_send();
        }

        vTaskDelay(pdMS_TO_TICKS(MQTT_REPORT_PERIOD_MS));
    }
}

//事件处理
static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) 
    {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            printf("[MQTT] connected to OneNET\n");

            // 每次重连都要重新订阅(clean session 会把订阅清掉)
            esp_mqtt_client_subscribe(event->client, TOPIC_PROP_SET, 1);
            esp_mqtt_client_subscribe(event->client, TOPIC_PROP_POST_REPLY, 1);
            printf("[MQTT] subscribe: %s\n", TOPIC_PROP_SET);

            // 任务名 : mqtt_report
            // 优先级 : 5   (main 任务 1, wifi 任务 23)
            // 栈     : 4096 字节
            // 绑核   : 不绑核, 调度器可放 CPU0 或 CPU1
            if (!s_report_task_started) 
            {
                s_report_task_started = true;
                xTaskCreate(mqtt_report_task, "mqtt_report", 4096, NULL, 5, NULL);//创建一个任务优先级为5的任务
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            printf("[MQTT] disconnected, auto reconnect in 5s ...\n");
            break;

        case MQTT_EVENT_DATA:
            if (event->topic != NULL &&
                event->topic_len == (int)strlen(TOPIC_PROP_SET) &&
                strncmp(event->topic, TOPIC_PROP_SET, event->topic_len) == 0) {
                handle_property_set(event->data, event->data_len);
            }
            else if (event->topic != NULL &&
                     event->topic_len == (int)strlen(TOPIC_PROP_POST_REPLY) &&
                     strncmp(event->topic, TOPIC_PROP_POST_REPLY, event->topic_len) == 0) {
                printf("[MQTT] post reply: %.*s\n", event->data_len, event->data);
            }
            break;

        case MQTT_EVENT_ERROR:
            printf("[MQTT] error, will reconnect\n");
            break;

        default:
            break;
    }
}

//启动 MQTT 客户端, 连接 OneNET(断线重连)
void mqtt_app_start(void)
{
    esp_mqtt_client_config_t cfg = 
    {
        .broker.address.uri                  = ONENET_URI,
        .credentials.client_id               = ONENET_DEVICE_NAME,  // Client ID = 设备名称
        .credentials.username                = ONENET_PRODUCT_ID,   // Username = 产品 ID
        .credentials.authentication.password = ONENET_TOKEN,        // Password = 设备 APIKey
        .session.keepalive                   = 60,
        .session.disable_clean_session       = false,

        //  ESP-MQTT 默认会在断线后自动重连, 设置重连间隔
        .network.disable_auto_reconnect      = false,
        .network.reconnect_timeout_ms        = 5000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        printf("[MQTT] client init failed\n");
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    // ------------- 任务创建点: MQTT 协议栈任务 -------------
    // esp_mqtt_client_start() 内部会创建 "mqtt_task":
    //   优先级 5, 栈 6144 字节, 不绑核
    // 我们的 mqtt_event_handler() 就是在这个任务里被调用的
    // ------------------------------------------------------
    esp_mqtt_client_start(s_client);//此处是创建了一个任务优先级为5的任务

    printf("[MQTT] start, broker: %s, client id: %s\n", ONENET_URI, ONENET_DEVICE_NAME);
}

//阻塞等待 MQTT 连接成功
bool mqtt_wait_connected(uint32_t timeout_ms)
{
    uint32_t waited = 0;

    while (!s_connected) 
    {
        if (timeout_ms != 0 && waited >= timeout_ms)
        {
            printf("[MQTT] connect timeout\n");
            return false;
        }
        printf(".");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }

    printf("\n");
    return true;
}

bool mqtt_is_connected(void)
{
    return s_connected;
}

bool net_is_ready(void)
{
    return s_connected;
}


//联网初始化 WiFi -> 等连上 -> SNTP 对时 -> 连 OneNET

void net_init(void)
{
    printf("\n[NET] wifi init ...\n");
    wifi_init_sta();

    if (!wifi_wait_connected(30000)) 
    {
        printf("[NET] wifi connect failed, check ssid/password\n");
        return;
    }
    printf("[NET] wifi ready\n");

    // 对时最多等 10 秒, 失败也继续, 不影响 MQTT
    time_sync_start();
    uint32_t waited = 0;
    while (!time_is_synced() && waited < 10000) 
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }

    mqtt_app_start();
    if (mqtt_wait_connected(20000)) 
    {
        printf("[NET] all ready\n");
    } else
     {
        printf("[NET] mqtt connect failed\n");
    }
}

//自定义上报
void mqtt_send_example(void)
{
    //f1
    mqtt_report_begin();
    mqtt_report_add_bool(s_relay_id[0], s_relay_state[0]);   //开关
    mqtt_report_add_int("test1", 123);                       //整数
    mqtt_report_add_float("temp", 26.5f);                    //小数
    mqtt_report_add_string("mode", "auto");                  //字符串
    mqtt_report_send();

    //f2
    mqtt_upload_bool("relay1", true); 
    mqtt_upload_int("test1", 100);      
    mqtt_upload_float("temp", 25.6f);  
}
