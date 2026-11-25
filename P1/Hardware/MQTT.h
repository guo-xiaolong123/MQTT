#ifndef __MQTT_H_
#define __MQTT_H_

#include <stdbool.h>
#include <stdint.h>

// ==================== OneNET 平台参数 ====================
// 控制台 -> 设备详情:
//   产品ID   : q8oyFaE9YD
//   设备名称 : HJ_Family
//   Token    : 用 AccessKey 生成的设备级 token (有有效期, 过期要重新生成)
#define ONENET_HOST         "mqtts.heclouds.com"        // 接入地址
#define ONENET_PRODUCT_ID   "q8oyFaE9YD"                // 产品 ID
#define ONENET_DEVICE_NAME  "HJ_Family"                 // 设备名称
#define ONENET_TOKEN        "version=2018-10-31&res=products%2Fq8oyFaE9YD%2Fdevices%2FHJ_Family&et=1858232738&method=md5&sign=Zq4a3JgO7R5oq%2BMzuX44yw%3D%3D"

// ==================== 物模型 Topic ====================
#define TOPIC_PROP_POST         "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post"
#define TOPIC_PROP_POST_REPLY   "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/post/reply"
#define TOPIC_PROP_SET          "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/set"
#define TOPIC_PROP_SET_REPLY    "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_NAME "/thing/property/set_reply"

// ==================== 属性标识符 ====================
// 必须和控制台里"产品开发 -> 物模型 -> 属性"的 identifier 一致,
// 继电器是 relay1 ~ relay4, 温湿度是 temp / humi。
#define RELAY_COUNT     4               // 继电器数量

#define PROP_TEMP       "temp"          // 温度: float
#define PROP_HUMI       "humi"          // 湿度: float

// 上报周期(毫秒), 想改上报频率改这里
#define MQTT_REPORT_PERIOD_MS   5000

// ==================== 温湿度数据 ====================
// 接上传感器后, 把读到的值赋给这两个变量, 上报时会自动带上去。
// 例如: g_temperature = dht_read_temp();
extern float g_temperature;
extern float g_humidity;

// ==================== 数值类型 ====================
typedef enum {
    MQTT_VAL_BOOL,      // 布尔
    MQTT_VAL_INT,       // 整数
    MQTT_VAL_FLOAT,     // 小数
    MQTT_VAL_STRING,    // 字符串
} mqtt_value_type_t;

typedef struct {
    mqtt_value_type_t type;         // 数据类型
    union {
        bool        b;
        int         i;
        double      f;
        const char *s;
    } v;
} mqtt_value_t;

// ==================== 继电器初始化 ====================
// 把 6 路继电器对应的 GPIO 全部配成输出并置为低电平(继电器断开)。
// app_main 里调用一次, 想改引脚改 MQTT.c 里的 s_relay_gpio[]。
void relay_init(void);

// 外部(串口/按键)设置某一路继电器: index 0~3, on=true 吸合
void relay_set(int index, bool on);

// 读取某一路继电器当前状态
bool relay_get(int index);

// ==================== 联网初始化(一个函数搞定) ====================
// 内部依次完成: 连接 WiFi -> SNTP 对时 -> 连接 OneNET
// app_main 里只需要调用一次 net_init();
void net_init(void);

// 联网是否就绪(OneNET 已连上)
bool net_is_ready(void);

// ==================== MQTT 底层接口 ====================
// 启动 MQTT 客户端(非阻塞), 一般由 net_init() 内部调用
void mqtt_app_start(void);

// 阻塞等待 MQTT 连上, 超时返回 false
bool mqtt_wait_connected(uint32_t timeout_ms);

// 查询 MQTT 是否已连接
bool mqtt_is_connected(void);

// ==================== 上报(可自由组合多个属性) ====================
void mqtt_report_begin(void);
void mqtt_report_add_bool(const char *id, bool value);
void mqtt_report_add_int(const char *id, int value);
void mqtt_report_add_float(const char *id, float value);
void mqtt_report_add_string(const char *id, const char *value);

// 发送这一包属性, 返回 >=0 成功(消息 id), <0 失败
int  mqtt_report_send(void);

// ==================== 单属性快捷上报 ====================
void mqtt_upload_bool(const char *id, bool value);
void mqtt_upload_int(const char *id, int value);
void mqtt_upload_float(const char *id, float value);

// 上报写法示例(可直接调用)
void mqtt_send_example(void);

// 回复云平台的属性设置请求(内部已自动调用, 一般不用管)
void mqtt_reply_set(const char *msg_id, int code, const char *msg);

#endif // __MQTT_H_
