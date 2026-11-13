#ifndef __MYAT_H
#define __MYAT_H

#include "Headfile.h"

/* ===================================================================
 * 4G模组 AT指令 -> OneNET(中国移动物联网平台) MQTT 接入
 * -------------------------------------------------------------------
 * 硬件连接:
 *   串口1(PA9/PA10) : 调试日志, 115200
 *   串口2(PA2/PA3)  : 4G模组,   115200
 *
 * 使用流程(全部在 main 的主循环里, 无需操作系统):
 *   1) 初始化: Task_Init() 里调用 AT_Init();
 *   2) 主循环: Task() 里调用 AT_Cloud_Task();
 *      -> 内部自动完成: AT / 注网 / MCONFIG / MIPSTART / MCONNECT
 *                      / MQTTMSGSET / MSUB, 并周期上传属性
 *   3) 想手动上传一次: 直接调用 AT_UploadParams();
 *
 * 要新增上传参数: 只改 myAT.c 里的 AT_UserParams() 函数, 里面已经
 * 留好"复制粘贴"的位置, 一行一个参数即可。
 * =================================================================== */

/* ==================== 1. 平台/模组参数(移植只改这里) ==================== */

/* 产品ID(OneNET 产品概况里的 产品ID) */
#define AT_ONENET_PRODUCT_ID        "97MlF47dAs"

/* 设备名称(OneNET 设备列表里的 设备名称) */
#define AT_ONENET_DEVICE_NAME       "car0"

/* 设备密钥(仅作记录: 用官方"token生成工具"算出下面的 AT_ONENET_TOKEN 即可,
 * 本工程不参与token的MD5/HMAC运算, 直接用算好的token字符串) */
#define AT_ONENET_AUTH_KEY          "YOUR_DEVICE_AUTH_KEY"

/* 设备鉴权token(用token生成工具算好后整串粘贴到这里)
 * 注意: et 是过期时间戳, 过期后平台会拒绝连接, 需要重新生成 */
#define AT_ONENET_TOKEN             "version=2018-10-31&res=products%2F97MlF47dAs%2Fdevices%2Fcar0&et=1834156958&method=md5&sign=xeeJLx7nzIcNjlaqw%2BMA%2Bw%3D%3D"

/* MQTT服务器与端口(OneNET 1883 明文 / 8883 加密) */
#define AT_MQTT_SERVER              "mqtts.heclouds.com"
#define AT_MQTT_PORT                1883

/* 连接参数 */
#define AT_MQTT_CLEAN_SESSION       1
#define AT_MQTT_KEEPALIVE           120         /* 心跳, 秒 */

/* ==================== 2. MQTT topic(由上面的ID自动拼装) ==================== */

/* 属性上报(设备 -> 平台) */
#define AT_TOPIC_PROPERTY_POST      "$sys/" AT_ONENET_PRODUCT_ID "/" AT_ONENET_DEVICE_NAME "/thing/property/post"
/* 属性设置(平台 -> 设备, 需要订阅) */
#define AT_TOPIC_PROPERTY_SET       "$sys/" AT_ONENET_PRODUCT_ID "/" AT_ONENET_DEVICE_NAME "/thing/property/set"
/* 属性上报回复 / 属性设置回复 */
#define AT_TOPIC_PROPERTY_POST_REPLY "$sys/" AT_ONENET_PRODUCT_ID "/" AT_ONENET_DEVICE_NAME "/thing/property/post/reply"
#define AT_TOPIC_PROPERTY_SET_REPLY  "$sys/" AT_ONENET_PRODUCT_ID "/" AT_ONENET_DEVICE_NAME "/thing/property/set/reply"

/* ==================== 3. 运行参数 ==================== */

#define AT_LOG_ENABLE               1           /* 1: 用串口1打印AT日志 */
#define AT_CMD_RETRY                3           /* 普通AT指令重试次数 */
#define AT_CMD_TIMEOUT_MS           1000        /* 普通AT指令超时 */
#define AT_CONNECT_TIMEOUT_MS       10000       /* 等待 CONNECT OK */
#define AT_CONNACK_TIMEOUT_MS       10000       /* 等待 CONNACK OK */
#define AT_SUBACK_TIMEOUT_MS        5000        /* 等待 SUBACK */
#define AT_PUB_TIMEOUT_MS           5000        /* 等待发布回复 */
#define AT_NET_TIMEOUT_MS           60000       /* 等待注网超时 */
#define AT_UPLOAD_PERIOD_MS         10000       /* 属性上传周期 */
#define AT_RECONNECT_INTERVAL_MS    10000       /* 掉线重连间隔 */
#define AT_AUTO_REPLY_SET           0           /* 1: 收到属性设置自动回复平台 */
#define AT_REPLY_DELAY_MS           2000         /* 回复前延时(ms), 0=立即回复
                                                 * 部分模组刚收完就发会掉线(CLOSED),
                                                 * 设 200~500 可避免 */

/* ==================== 4. 返回值 ==================== */

typedef enum
{
	AT_OK = 0,          /* 成功 */
	AT_ERR_TIMEOUT,     /* 等待回复超时 */
	AT_ERR_RESP,        /* 模组返回 ERROR/FAIL */
	AT_ERR_PARAM,       /* 数据过长/参数错误, 需要加大缓冲区 */
	AT_ERR_STATE        /* 当前未连接云平台 */
} AT_Status;

/* ==================== 5. 对外函数 ==================== */

/* 初始化AT驱动(初始化内部状态, 并确保串口2接收已开启) */
void      AT_Init(void);

/* 一条龙连接云平台: AT -> 注网 -> MCONFIG -> MIPSTART -> MCONNECT
 *                     -> MQTTMSGSET -> MSUB, 返回 AT_OK 表示全部成功 */
AT_Status AT_Connect(void);

/* 断开MQTT */
void      AT_Disconnect(void);

/* 是否已连接云平台 */
uint8_t   AT_IsConnected(void);

/* 发送一条AT指令并等待期望回复(内部自动重试+串口1日志)
 * cmd    : 不含"\r\n"的指令
 * expect : 期望出现在"行首"的关键字, 如 "OK" / "CONNECT OK" / "SUBACK"
 * 返回 AT_OK 表示本条指令成功 */
AT_Status AT_SendCmd(const char *cmd, const char *expect, uint16_t timeout_ms, uint8_t retry);

/* 集成接口: 组装 AT_UserParams() 里的参数并上报到云平台(手动调用一次) */
AT_Status AT_UploadParams(void);

/* 集成接口: 放进主循环即可 -> 自动重连 + 周期上传 + 解析平台下发 */
void      AT_Cloud_Task(void);

/* 只处理模组主动上报(平台下发属性设置等) */
void      AT_Task(void);

/* ==================== 6. 参数拼接(给 AT_UserParams 用) ==================== */

void AT_Param_Reset(void);                                       /* 清空参数 */
void AT_Param_Int(const char *name, int32_t value);              /* {"value":整数} */
void AT_Param_UInt(const char *name, uint32_t value);            /* {"value":无符号整数} */
void AT_Param_Float(const char *name, double value, uint8_t decimals); /* 小数位数0~6 */
void AT_Param_Bool(const char *name, uint8_t value);             /* {"value":true/false} */
void AT_Param_Str(const char *name, const char *value);          /* {"value":"字符串"} */
void AT_Param_Raw(const char *json_member);                      /* 原样拼接一个JSON成员 */

/* ==================== 7. 平台下发数据 ==================== */

/* 最近一次收到的属性设置JSON(已去掉首尾换行), 没有收到过返回空串 */
const char *AT_GetDownlink(void);
uint16_t    AT_GetDownlinkLen(void);

/* 累计成功上传次数 */
uint32_t    AT_GetUploadCount(void);

/* ==================== 8. 平台下发属性接收(判断是哪个 -> 赋值) ==================== */

#define AT_RECV_NAME_LEN        24U     /* 属性名最大长度 */
#define AT_RECV_STR_LEN         32U     /* 字符串属性值最大长度 */

/* 下发属性的值类型 */
typedef enum
{
	AT_VAL_INT = 0,     /* 整数 */
	AT_VAL_FLOAT,       /* 小数 */
	AT_VAL_BOOL,        /* 布尔 */
	AT_VAL_STR          /* 字符串 */
} AT_ValueType;

/* 解析出来的一个属性值 */
typedef struct
{
	AT_ValueType type;                  /* 实际类型 */
	int32_t      i;                     /* 整数/布尔值 */
	double       f;                     /* 小数值(整数也会填好) */
	char         s[AT_RECV_STR_LEN];    /* 字符串值 */
} AT_Value;

/* -------------------------------------------------------------------
 * 平台下发属性后自动更新的变量(在 myAT.c 的"接收变量区"里定义)
 * ------------------------------------------------------------------- */

/* 第一类: 0/1 开关量(uint8_t, 平台下发 1/0 或 true/false) */
extern uint8_t           g_recv_sw1;
extern uint8_t           g_recv_sw2;
extern uint8_t           g_recv_sw3;
extern uint8_t           g_recv_sw4;

/* 第二类: 整数(int32_t) */
extern int32_t           g_recv_num1;
extern int32_t           g_recv_num2;
extern int32_t           g_recv_num3;
extern int32_t           g_recv_num4;

/* 原来的 test1 / test2 属性(整数) */
extern int32_t           g_recv_test1;
extern int32_t           g_recv_test2;

/* 最近一次被赋值的属性名(判断是哪个属性) */
extern char              g_recv_name[AT_RECV_NAME_LEN];

/* 1: 收到了新的下发并已赋值, 用户处理完后调用 AT_ClearRecvUpdated() 清零 */
extern volatile uint8_t  g_recv_updated;

/* 弱函数: 每解析到一个下发属性都会调用一次(name=属性名, val=属性值)
 * 默认实现什么都不做;
 * 需要做动作(亮灯/调速/切换模式)时, 在 fun.c 里重新定义同名函数即可覆盖,
 * 例如:
 *   void AT_OnRecvProperty(const char *name, const AT_Value *val)
 *   {
 *       if (strcmp(name, "led") == 0) { LED_Set(val->i); }
 *   }
 */
void AT_OnRecvProperty(const char *name, const AT_Value *val);

/* 清除"有新下发"标志 */
void AT_ClearRecvUpdated(void);

#endif
