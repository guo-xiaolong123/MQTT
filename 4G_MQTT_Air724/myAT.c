#include "Headfile.h"
#include <stdlib.h>

/* ===================================================================
 * AT指令  ->  OneNET MQTT 接入实现
 * -------------------------------------------------------------------
 * 完整连接流程(与串口日志完全一致):
 *   AT                                   -> OK
 *   AT+CGREG?                            -> +CGREG: 0,1  (已注网)
 *   AT+CGATT?                            -> +CGATT: 1    (已附着)
 *   AT+MCONFIG="设备名","产品ID","token"   -> OK
 *   AT+MIPSTART="mqtts.heclouds.com",1883 -> OK / CONNECT OK
 *   AT+MCONNECT=1,120                    -> OK / CONNACK OK
 *   AT+MQTTMSGSET=0                      -> OK            (明文收发)
 *   AT+MSUB=".../property/set",0          -> OK / SUBACK
 *   AT+MPUB=".../property/post",0,0,"..." -> OK           (上传属性)
 *
 * 所有步骤都用串口1打印日志, 每一步都会校验模组回复是否成功。
 * =================================================================== */

/* ==================== 内部缓冲区(单位: 字节) ==================== */

#define AT_RESP_BUF_SIZE        512U    /* 指令回复缓存 */
#define AT_PARAMS_BUF_SIZE      384U    /* 参数拼接缓存 */
#define AT_JSON_BUF_SIZE        512U    /* 完整JSON缓存 */
#define AT_CMD_BUF_SIZE         1280U   /* 完整AT指令缓存(含转义后的JSON) */
#define AT_URC_BUF_SIZE         512U    /* 模组主动上报解析缓存(下发JSON) */

/* ==================== 内部状态 ==================== */

static uint8_t  at_connected   = 0;     /* 1: MQTT已连接 */
static uint8_t  at_conn_tried  = 0;     /* 是否尝试过连接 */
static uint32_t at_conn_tick   = 0;     /* 上次连接尝试时刻 */
static uint32_t at_upload_tick = 0;     /* 上次上传时刻 */
static uint32_t at_msg_id      = 1;     /* 上报消息id(每次+1) */
static uint32_t at_upload_cnt  = 0;     /* 上传成功次数 */
static uint8_t  at_pub_fail    = 0;     /* 连续上传失败次数 */

static char     at_resp[AT_RESP_BUF_SIZE];      /* 指令回复 */
static uint16_t at_resp_len = 0;

static char     at_params[AT_PARAMS_BUF_SIZE];  /* "xx":{"value":1},... */
static uint16_t at_params_len = 0;
static uint8_t  at_param_cnt  = 0;
static uint8_t  at_params_ok  = 1;

static char     at_json[AT_JSON_BUF_SIZE];      /* {"id":"1","params":{...}} */
static char     at_cmd[AT_CMD_BUF_SIZE];        /* 最终AT指令 */

static char     at_downlink[AT_URC_BUF_SIZE];   /* 平台下发内容 */
static uint16_t at_downlink_len = 0;

/* ==================== 串口1日志 ==================== */

/* 短日志(格式化), 走串口1 */
static void AT_Log(const char *fmt, ...)
{
	char buf[128];
	va_list args;

	if (!AT_LOG_ENABLE)
	{
		return;
	}

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	USART1_Printf("%s", buf);
}

/* 长日志(指令/JSON可能超过255字节), 分段打印, 走串口1 */
static void AT_LogLong(const char *prefix, const char *str)
{
	char seg[128];
	uint16_t i;

	if (!AT_LOG_ENABLE)
	{
		return;
	}
	if (str == NULL)
	{
		return;
	}

	USART1_Printf("%s", prefix);
	while (*str != '\0')
	{
		i = 0;
		while (*str != '\0' && i < (sizeof(seg) - 1U))
		{
			seg[i++] = *str++;
		}
		seg[i] = '\0';
		USART1_Printf("%s", seg);
	}
	USART1_Printf("\r\n");
}

/* ==================== 回复缓存 ==================== */

static void AT_RespClear(void)
{
	at_resp_len = 0;
	at_resp[0] = '\0';
}

/* 判断回复里是否出现关键字(要求关键字位于行首, 避免匹配到指令回显) */
static uint8_t AT_RespHas(const char *expect)
{
	const char *p;

	if (expect == NULL)
	{
		return 0;
	}

	p = at_resp;
	while ((p = strstr(p, expect)) != NULL)
	{
		if (p == at_resp || p[-1] == '\n')
		{
			return 1;
		}
		p++;
	}
	return 0;
}

/* 模组报错判断 */
static uint8_t AT_RespError(void)
{
	if (AT_RespHas("ERROR") != 0)
	{
		return 1;
	}
	if (AT_RespHas("CONNECT FAIL") != 0)
	{
		return 1;
	}
	if (AT_RespHas("+CME ERROR") != 0)
	{
		return 1;
	}
	if (AT_RespHas("+CMS ERROR") != 0)
	{
		return 1;
	}
	return 0;
}

/* 等待期望回复, 期间不断读取串口2并打印到串口1 */
static AT_Status AT_WaitFor(const char *expect, uint16_t timeout_ms)
{
	uint32_t start_tick = HAL_GetTick();
	uint16_t room;
	uint16_t n;
	uint16_t old_len;

	for (;;)
	{
		room = (uint16_t)(AT_RESP_BUF_SIZE - 1U - at_resp_len);
		if (room <= 1U)
		{
			/* 缓存已满还没匹配到, 丢掉重新累积, 避免卡死 */
			at_resp_len = 0;
			at_resp[0] = '\0';
			room = (uint16_t)(AT_RESP_BUF_SIZE - 1U);
		}

		old_len = at_resp_len;
		n = USART2_ReadChunk((uint8_t *)&at_resp[old_len], room);
		if (n > 0)
		{
			at_resp_len = (uint16_t)(old_len + n);
			at_resp[at_resp_len] = '\0';
			AT_LogLong("[AT] <-- ", &at_resp[old_len]);
		}

		if (AT_RespHas(expect) != 0)
		{
			return AT_OK;
		}
		if (AT_RespError() != 0)
		{
			return AT_ERR_RESP;
		}
		if ((HAL_GetTick() - start_tick) >= timeout_ms)
		{
			return AT_ERR_TIMEOUT;
		}
	}
}

/* ==================== 指令发送 ==================== */

AT_Status AT_SendCmd(const char *cmd, const char *expect, uint16_t timeout_ms, uint8_t retry)
{
	uint8_t i;
	AT_Status st = AT_ERR_TIMEOUT;

	if (cmd == NULL)
	{
		return AT_ERR_PARAM;
	}
	if (retry == 0)
	{
		retry = 1;
	}

	for (i = 0; i < retry; i++)
	{
		USART2_Flush();          /* 丢掉上一条残留数据 */
		AT_RespClear();

		AT_Log("[AT] --> ");
		AT_LogLong("", cmd);

		USART2_SendString(cmd);
		USART2_SendString("\r\n");

		st = AT_WaitFor(expect, timeout_ms);
		if (st == AT_OK)
		{
			AT_Log("[AT] <-- OK   (%s)\r\n", expect);
			return AT_OK;
		}

		AT_Log("[AT] <-- %s  (%u/%u) %s\r\n",
		       (st == AT_ERR_TIMEOUT) ? "TIMEOUT" : "FAIL",
		       (unsigned)(i + 1U), (unsigned)retry, expect);

		HAL_Delay(200);
	}

	return st;
}

/* ==================== 参数拼接 ==================== */

static void AT_AppendChar(char c)
{
	if (at_params_len < (AT_PARAMS_BUF_SIZE - 1U))
	{
		at_params[at_params_len++] = c;
		at_params[at_params_len] = '\0';
	}
	else
	{
		at_params_ok = 0;    /* 超出缓存, 整包作废 */
	}
}

static void AT_AppendText(const char *s)
{
	if (s == NULL)
	{
		return;
	}
	while (*s != '\0')
	{
		AT_AppendChar(*s++);
	}
}

/* 拼接 "name":  并自动加逗号 */
static uint8_t AT_Param_Begin(const char *name)
{
	if (name == NULL)
	{
		at_params_ok = 0;
		return 0;
	}

	if (at_param_cnt > 0)
	{
		AT_AppendChar(',');
	}
	AT_AppendChar('"');
	AT_AppendText(name);
	AT_AppendChar('"');
	AT_AppendChar(':');
	at_param_cnt++;

	return at_params_ok;
}

void AT_Param_Reset(void)
{
	at_params_len = 0;
	at_param_cnt  = 0;
	at_params_ok  = 1;
	at_params[0]  = '\0';
}

/* 整数: "name":{"value":123} */
void AT_Param_Int(const char *name, int32_t value)
{
	char num[16];

	snprintf(num, sizeof(num), "%ld", (long)value);

	if (!AT_Param_Begin(name))
	{
		return;
	}
	AT_AppendText("{\"value\":");
	AT_AppendText(num);
	AT_AppendText("}");
}

/* 无符号整数: "name":{"value":123} */
void AT_Param_UInt(const char *name, uint32_t value)
{
	char num[16];

	snprintf(num, sizeof(num), "%lu", (unsigned long)value);

	if (!AT_Param_Begin(name))
	{
		return;
	}
	AT_AppendText("{\"value\":");
	AT_AppendText(num);
	AT_AppendText("}");
}

/* 小数: "name":{"value":25.6}  (不使用%f, 兼容各种C库, 四舍五入) */
void AT_Param_Float(const char *name, double value, uint8_t decimals)
{
	char num[24];
	char frac[8];
	uint32_t ipart;
	uint32_t fp;
	uint32_t pow10 = 1;
	uint8_t i;
	uint8_t neg = 0;

	if (decimals > 6U)
	{
		decimals = 6U;
	}
	if (value < 0.0)
	{
		neg = 1;
		value = -value;
	}
	if (value > 2000000000.0)
	{
		value = 2000000000.0;
	}

	for (i = 0; i < decimals; i++)
	{
		pow10 *= 10U;
	}

	ipart = (uint32_t)value;
	fp = (uint32_t)((value - (double)ipart) * (double)pow10 + 0.5);
	if (fp >= pow10)
	{
		ipart++;
		fp -= pow10;
	}

	if (decimals == 0U)
	{
		snprintf(num, sizeof(num), "%s%lu", (neg != 0) ? "-" : "", (unsigned long)ipart);
	}
	else
	{
		for (i = decimals; i > 0U; i--)
		{
			frac[i - 1U] = (char)('0' + (char)(fp % 10U));
			fp /= 10U;
		}
		frac[decimals] = '\0';
		snprintf(num, sizeof(num), "%s%lu.%s", (neg != 0) ? "-" : "",
		         (unsigned long)ipart, frac);
	}

	if (!AT_Param_Begin(name))
	{
		return;
	}
	AT_AppendText("{\"value\":");
	AT_AppendText(num);
	AT_AppendText("}");
}

/* 布尔: "name":{"value":true} */
void AT_Param_Bool(const char *name, uint8_t value)
{
	if (!AT_Param_Begin(name))
	{
		return;
	}
	AT_AppendText("{\"value\":");
	AT_AppendText((value != 0) ? "true" : "false");
	AT_AppendText("}");
}

/* 字符串: "name":{"value":"abc"}  (自动转义 " 和 \ ) */
void AT_Param_Str(const char *name, const char *value)
{
	char c;

	if (!AT_Param_Begin(name))
	{
		return;
	}

	AT_AppendText("{\"value\":\"");
	while (value != NULL && *value != '\0')
	{
		c = *value++;
		if (c == '"' || c == '\\')
		{
			AT_AppendChar('\\');
		}
		if ((uint8_t)c < 0x20U)
		{
			continue;    /* 去掉控制字符 */
		}
		AT_AppendChar(c);
	}
	AT_AppendText("\"}");
}

/* 原样拼接一个JSON成员, 例如:
 * AT_Param_Raw("\"test3\":{\"value\":123}"); */
void AT_Param_Raw(const char *json_member)
{
	if (json_member == NULL)
	{
		at_params_ok = 0;
		return;
	}

	if (at_param_cnt > 0)
	{
		AT_AppendChar(',');
	}
	AT_AppendText(json_member);
	at_param_cnt++;
}

/* ===================================================================
 * 用户参数区: 要上传什么参数, 就在这里复制/粘贴一行
 * -------------------------------------------------------------------
 * 平台(OneNET)上已经创建的属性, 这里的 name 必须与属性标识符一致;
 * 每个参数最终会拼成 "name":{"value":值} 的形式放进 params。
 * =================================================================== */
static void AT_UserParams(void)
{
	/* ---------------- 在这里添加/复制粘贴参数 ---------------- */
	AT_Param_Int("test1", 13);              /* 属性 test1 */
	AT_Param_Int("test2", 14);              /* 属性 test2 */

	/* 小数(第3个参数是小数位数): */
	/* AT_Param_Float("temperature", 25.6, 1); */

	/* 字符串: */
	/* AT_Param_Str("status", "online"); */

	/* 布尔: */
	/* AT_Param_Bool("run", 1); */

	/* 想完全自己写JSON成员时: */
	/* AT_Param_Raw("\"test3\":{\"value\":123}"); */
	/* ---------------- 参数添加结束 ---------------- */
}

/* ==================== 组包 / 上传 ==================== */

/* 生成 {"id":"1","params":{...}} */
static uint16_t AT_JsonBuild(char *out, uint16_t max)
{
	int n;

	AT_Param_Reset();
	AT_UserParams();

	if (!at_params_ok)
	{
		AT_Log("[AT] params overflow! enlarge AT_PARAMS_BUF_SIZE\r\n");
		return 0;
	}

	n = snprintf(out, max, "{\"id\":\"%lu\",\"params\":{%s}}",
	             (unsigned long)at_msg_id, at_params);
	if (n < 0 || n >= (int)max)
	{
		AT_Log("[AT] json overflow! enlarge AT_JSON_BUF_SIZE\r\n");
		return 0;
	}

	return (uint16_t)n;
}

/* 把JSON里的 " 转成 \22, \ 转成 \5c, 直接拼进AT指令
 * 结果形如:  AT+MPUB="topic",0,0,"{\22id\22:\221\22,...}" */
static uint8_t AT_BuildPubCmd(char *cmd, uint16_t max, const char *topic,
                              uint8_t qos, uint8_t retain, const char *payload)
{
	uint16_t pos;
	int n;

	n = snprintf(cmd, max, "AT+MPUB=\"%s\",%u,%u,\"",
	             topic, (unsigned)qos, (unsigned)retain);
	if (n < 0 || n >= (int)max)
	{
		return 0;
	}
	pos = (uint16_t)n;

	while (payload != NULL && *payload != '\0')
	{
		if (*payload == '"')
		{
			if ((uint16_t)(pos + 3U) >= max)
			{
				return 0;
			}
			cmd[pos++] = '\\';
			cmd[pos++] = '2';
			cmd[pos++] = '2';
		}
		else if (*payload == '\\')
		{
			if ((uint16_t)(pos + 3U) >= max)
			{
				return 0;
			}
			cmd[pos++] = '\\';
			cmd[pos++] = '5';
			cmd[pos++] = 'c';
		}
		else
		{
			if ((uint16_t)(pos + 1U) >= max)
			{
				return 0;
			}
			cmd[pos++] = *payload;
		}
		payload++;
	}

	if ((uint16_t)(pos + 1U) >= max)
	{
		return 0;
	}
	cmd[pos++] = '"';
	cmd[pos]   = '\0';

	return 1;
}

/* 上报一次属性到云平台 */
AT_Status AT_UploadParams(void)
{
	AT_Status st;
	uint16_t json_len;

	if (!at_connected)
	{
		AT_Log("[AT] upload skipped: not connected\r\n");
		return AT_ERR_STATE;
	}

	/* 1. 组装参数 -> {"id":"x","params":{...}} */
	json_len = AT_JsonBuild(at_json, (uint16_t)sizeof(at_json));
	if (json_len == 0)
	{
		return AT_ERR_PARAM;
	}

	/* 2. 转义并拼成 AT+MPUB 指令 */
	if (!AT_BuildPubCmd(at_cmd, (uint16_t)sizeof(at_cmd), AT_TOPIC_PROPERTY_POST,
	                    0, 0, at_json))
	{
		AT_Log("[AT] publish cmd too long! enlarge AT_CMD_BUF_SIZE\r\n");
		return AT_ERR_PARAM;
	}

	AT_Log("[AT] json(%u): ", (unsigned)json_len);
	AT_LogLong("", at_json);

	/* 3. 发送并校验回复 */
	st = AT_SendCmd(at_cmd, "OK", AT_PUB_TIMEOUT_MS, 1);
	if (st == AT_OK)
	{
		at_msg_id++;
		at_upload_cnt++;
		at_pub_fail = 0;
		at_upload_tick = HAL_GetTick();
		AT_Log("[AT] upload #%lu OK\r\n", (unsigned long)at_upload_cnt);
	}
	else
	{
		AT_Log("[AT] upload FAIL (%d)\r\n", (int)st);
		at_pub_fail++;
		if (at_pub_fail >= 3U)
		{
			/* 连续失败, 认为连接已掉线, 下次循环重新连接 */
			at_pub_fail = 0;
			at_connected = 0;
			AT_Log("[AT] too many upload fails, will reconnect\r\n");
		}
	}

	return st;
}

/* ==================== 连接云平台 ==================== */

/* 从回复中解析 +CGREG: <n>,<stat> 的 stat */
static uint8_t AT_ParseCGREG(uint8_t *stat)
{
	const char *p;

	p = strstr(at_resp, "+CGREG:");
	if (p == NULL)
	{
		return 0;
	}
	p = strchr(p, ',');          /* 跳过 <n> */
	if (p == NULL)
	{
		return 0;
	}
	p++;
	*stat = (uint8_t)strtoul(p, NULL, 10);
	return 1;
}

/* 从回复中解析 +CGATT: <state> */
static uint8_t AT_ParseCGATT(uint8_t *att)
{
	const char *p;

	p = strstr(at_resp, "+CGATT:");
	if (p == NULL)
	{
		return 0;
	}
	p += 7;
	while (*p == ' ')
	{
		p++;
	}
	*att = (uint8_t)strtoul(p, NULL, 10);
	return 1;
}

/* 等待注网 + GPRS附着 */
static AT_Status AT_WaitNetwork(void)
{
	uint32_t start_tick = HAL_GetTick();
	uint8_t reg = 0;
	uint8_t att = 0;

	AT_Log("[AT] waiting network registration...\r\n");

	for (;;)
	{
		USART2_Flush();
		AT_RespClear();
		USART2_SendString("AT+CGREG?\r\n");

		if (AT_WaitFor("OK", AT_CMD_TIMEOUT_MS) == AT_OK &&
		    AT_ParseCGREG(&reg) != 0)
		{
			AT_Log("[AT] CGREG stat = %u\r\n", (unsigned)reg);

			if (reg == 1U || reg == 5U)      /* 1:本地网 5:漫游 */
			{
				AT_RespClear();
				USART2_Flush();
				USART2_SendString("AT+CGATT?\r\n");

				if (AT_WaitFor("OK", AT_CMD_TIMEOUT_MS) == AT_OK &&
				    AT_ParseCGATT(&att) != 0 && att == 1U)
				{
					AT_Log("[AT] CGATT = 1, network ready\r\n");
					return AT_OK;
				}
				AT_Log("[AT] CGATT not ready (%u)\r\n", (unsigned)att);
			}
		}

		if ((HAL_GetTick() - start_tick) >= AT_NET_TIMEOUT_MS)
		{
			AT_Log("[AT] network register timeout!\r\n");
			return AT_ERR_TIMEOUT;
		}
		HAL_Delay(1000);
	}
}

/* 清理上一次可能残留的MQTT连接(掉线重连时用), 失败不影响后面流程 */
static void AT_CleanSession(void)
{
	AT_Log("[AT] clean old session\r\n");

	AT_RespClear();
	USART2_Flush();
	USART2_SendString("AT+MDISCONNECT\r\n");
	(void)AT_WaitFor("OK", 500);      /* 没连接过会回ERROR, 忽略即可 */

	AT_RespClear();
	USART2_Flush();
}

/* 一条龙连接云平台 */
AT_Status AT_Connect(void)
{
	AT_Status st;

	if (at_connected)
	{
		return AT_OK;
	}

	AT_Log("\r\n[AT] ===== connect OneNET begin =====\r\n");

	/* 1. 检查模组是否在线 */
	st = AT_SendCmd("AT", "OK", AT_CMD_TIMEOUT_MS, AT_CMD_RETRY);
	if (st != AT_OK)
	{
		AT_Log("[AT] module no reply! check USART2 wiring/baudrate\r\n");
		return st;
	}

	/* 2. 清理旧连接(上一次掉线后模组可能还留着连接) */
	AT_CleanSession();

	/* 3. 等待注网 */
	st = AT_WaitNetwork();
	if (st != AT_OK)
	{
		return st;
	}

	/* 4. 设备三元组配置(设备名 / 产品ID / token) */
	snprintf(at_cmd, sizeof(at_cmd), "AT+MCONFIG=\"%s\",\"%s\",\"%s\"",
	         AT_ONENET_DEVICE_NAME, AT_ONENET_PRODUCT_ID, AT_ONENET_TOKEN);
	st = AT_SendCmd(at_cmd, "OK", AT_CMD_TIMEOUT_MS, AT_CMD_RETRY);
	if (st != AT_OK)
	{
		return st;
	}

	/* 5. 连接MQTT服务器 */
	snprintf(at_cmd, sizeof(at_cmd), "AT+MIPSTART=\"%s\",%d",
	         AT_MQTT_SERVER, AT_MQTT_PORT);
	st = AT_SendCmd(at_cmd, "CONNECT OK", AT_CONNECT_TIMEOUT_MS, 2);
	if (st != AT_OK)
	{
		return st;
	}

	/* 6. MQTT CONNECT(clean session, keepalive) */
	snprintf(at_cmd, sizeof(at_cmd), "AT+MCONNECT=%d,%d",
	         AT_MQTT_CLEAN_SESSION, AT_MQTT_KEEPALIVE);
	st = AT_SendCmd(at_cmd, "CONNACK OK", AT_CONNACK_TIMEOUT_MS, 2);
	if (st != AT_OK)
	{
		return st;
	}

	/* 7. 消息格式设为明文(否则下发是十六进制) */
	st = AT_SendCmd("AT+MQTTMSGSET=0", "OK", AT_CMD_TIMEOUT_MS, AT_CMD_RETRY);
	if (st != AT_OK)
	{
		return st;
	}

	/* 8. 订阅属性设置topic */
	snprintf(at_cmd, sizeof(at_cmd), "AT+MSUB=\"%s\",0", AT_TOPIC_PROPERTY_SET);
	st = AT_SendCmd(at_cmd, "SUBACK", AT_SUBACK_TIMEOUT_MS, 2);
	if (st != AT_OK)
	{
		return st;
	}

	at_connected = 1;
	at_pub_fail  = 0;
	at_upload_tick = HAL_GetTick();

	AT_Log("[AT] ===== connect OK, upload every %u ms =====\r\n",
	       (unsigned)AT_UPLOAD_PERIOD_MS);
	return AT_OK;
}

/* 断开MQTT */
void AT_Disconnect(void)
{
	if (at_connected)
	{
		AT_SendCmd("AT+MDISCONNECT", "OK", AT_CMD_TIMEOUT_MS, 1);
	}
	at_connected = 0;
}

uint8_t AT_IsConnected(void)
{
	return at_connected;
}

/* ==================== 平台下发解析 ==================== */

#if (AT_AUTO_REPLY_SET != 0)
/* 待回复的"属性设置"消息(延时发送, 避免模组刚收完就发导致CLOSED) */
static char     at_reply_id[24] = "";
static uint8_t  at_reply_pending = 0;
static uint32_t at_reply_tick = 0;

/* 从下发JSON里取 "id":"xxx" 的 xxx */
static uint8_t AT_ParseMsgId(const char *json, char *id, uint16_t max)
{
	const char *p;
	const char *q;
	uint16_t n;

	p = strstr(json, "\"id\"");
	if (p == NULL)
	{
		return 0;
	}
	p = strchr(p, ':');
	if (p == NULL)
	{
		return 0;
	}
	p++;
	while (*p == ' ')
	{
		p++;
	}
	if (*p != '"')
	{
		return 0;
	}
	p++;
	q = strchr(p, '"');
	if (q == NULL)
	{
		return 0;
	}

	n = (uint16_t)(q - p);
	if (n >= max)
	{
		n = (uint16_t)(max - 1U);
	}
	memcpy(id, p, n);
	id[n] = '\0';

	return 1;
}

/* 回复属性设置(否则云平台会提示设备未回复)
 * 注意: 不在这里立即发送, 而是由 AT_Cloud_Task() 延时发送,
 *       因为部分模组"刚收完下发马上发布"会把MQTT连接断掉(打印CLOSED) */
static void AT_ReplyPropertySet(void)
{
	if (!at_connected || at_reply_id[0] == '\0')
	{
		return;
	}

	snprintf(at_json, sizeof(at_json),
	         "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}", at_reply_id);

	if (!AT_BuildPubCmd(at_cmd, (uint16_t)sizeof(at_cmd),
	                    AT_TOPIC_PROPERTY_SET_REPLY, 0, 0, at_json))
	{
		AT_Log("[AT] reply cmd too long\r\n");
		return;
	}

	AT_Log("[AT] reply property/set, id=%s\r\n", at_reply_id);
	(void)AT_SendCmd(at_cmd, "OK", AT_PUB_TIMEOUT_MS, 1);
}
#endif

/* ===================================================================
 * 平台下发属性接收: 判断是哪个属性 -> 赋值给对应变量
 * -------------------------------------------------------------------
 * OneNET 属性设置下发格式:
 *   +MSUB: "$sys/{pid}/{dev}/thing/property/set",59 byte,
 *          {"id":"1","version":"1.0","params":{"sw1":1,"num1":123}}
 * 也兼容: {"id":"1","params":{"sw1":{"value":1}}}
 *
 * 收到后自动按 at_recv_table 判断属性名并赋值, 串口1打印:
 *   [AT] recv sw1 = 1
 *   [AT] recv num1 = 123
 * =================================================================== */

/* ===================================================================
 * 接收变量区
 * -------------------------------------------------------------------
 * 第一类: 0/1 开关量
 *   平台下发 1/0, 或 true/false, 或 "1" 都可以;
 *   变量必须是 uint8_t, 表中类型写 AT_VAL_BOOL。
 * =================================================================== */
uint8_t g_recv_sw1 = 0;      /* 把 sw1 改成你平台上的属性标识符 */
uint8_t g_recv_sw2 = 0;
uint8_t g_recv_sw3 = 0;
uint8_t g_recv_sw4 = 0;

/* 复制下面这一行, 改个名字就多一个开关量:
 * uint8_t g_recv_sw5 = 0;
 */

/* ===================================================================
 * 第二类: 整数
 *   平台下发 123 / -5 这类;
 *   变量必须是 int32_t, 表中类型写 AT_VAL_INT。
 * =================================================================== */
int32_t g_recv_num1 = 0;
int32_t g_recv_num2 = 0;
int32_t g_recv_num3 = 0;
int32_t g_recv_num4 = 0;

/* 复制下面这一行, 改个名字就多一个整数:
 * int32_t g_recv_num5 = 0;
 */

/* 原来 OneNET 上的 test1 / test2 属性(整数) */
int32_t g_recv_test1 = 0;
int32_t g_recv_test2 = 0;

char g_recv_name[AT_RECV_NAME_LEN] = "";
volatile uint8_t g_recv_updated = 0;

/* ===================================================================
 * 属性名 -> 变量 映射表
 * -------------------------------------------------------------------
 * 格式: { "平台属性标识符", 类型, &变量, 0 }
 * 只有字符串类型 AT_VAL_STR 才需要第4个参数(目标缓冲区大小)。
 * 属性名写错不会赋值, 只会打印 [AT] recv unknown property: xxx
 * =================================================================== */
static const struct
{
	const char  *name;
	AT_ValueType type;
	void        *dst;
	uint16_t     size;
} at_recv_table[] =
{
	/* ---------------- 第一类: 0/1 开关量 ---------------- */
	{ "sw1", AT_VAL_BOOL, &g_recv_sw1, 0 },
	{ "sw2", AT_VAL_BOOL, &g_recv_sw2, 0 },
	{ "sw3", AT_VAL_BOOL, &g_recv_sw3, 0 },
	{ "sw4", AT_VAL_BOOL, &g_recv_sw4, 0 },

	/* 复制下面这一行, 把 "sw5" 改成平台上的属性标识符:
	 * { "sw5", AT_VAL_BOOL, &g_recv_sw5, 0 },
	 */

	/* ---------------- 第二类: 整数 ---------------- */
	{ "num1", AT_VAL_INT, &g_recv_num1, 0 },
	{ "num2", AT_VAL_INT, &g_recv_num2, 0 },
	{ "num3", AT_VAL_INT, &g_recv_num3, 0 },
	{ "num4", AT_VAL_INT, &g_recv_num4, 0 },

	/* 复制下面这一行, 把 "num5" 改成平台上的属性标识符:
	 * { "num5", AT_VAL_INT, &g_recv_num5, 0 },
	 */

	/* ---------------- 原来的示例属性 ---------------- */
	{ "test1", AT_VAL_INT, &g_recv_test1, 0 },
	{ "test2", AT_VAL_INT, &g_recv_test2, 0 }
};

/* 弱函数默认实现: 需要在下发时做动作(亮灯/继电器/调速), 在 fun.c 里
 * 重新定义同名函数覆盖它即可, 变量已经自动赋值好了。例如:
 *
 *   void AT_OnRecvProperty(const char *name, const AT_Value *val)
 *   {
 *       if (strcmp(name, "sw1") == 0)
 *       {
 *           HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13,
 *                             g_recv_sw1 ? GPIO_PIN_RESET : GPIO_PIN_SET);
 *       }
 *       else if (strcmp(name, "num1") == 0)
 *       {
 *           Motor_SetSpeed((uint16_t)g_recv_num1);
 *       }
 *   }
 */
__weak void AT_OnRecvProperty(const char *name, const AT_Value *val)
{
	(void)name;
	(void)val;
}

void AT_ClearRecvUpdated(void)
{
	g_recv_updated = 0;
}

/* 打印小数(不使用 %f, 兼容 microlib), 保留2位 */
static void AT_LogFloat(const char *prefix, const char *name, double v)
{
	int32_t scaled;
	int32_t ip;
	int32_t fp;

	scaled = (int32_t)(v * 100.0 + ((v >= 0.0) ? 0.5 : -0.5));
	ip = scaled / 100;
	fp = scaled % 100;
	if (fp < 0)
	{
		fp = -fp;
	}
	AT_Log("%s%s = %ld.%02ld\r\n", prefix, name, (long)ip, (long)fp);
}

/* ---------- 精简JSON解析(只取 params 里的键值对) ---------- */

static const char *AT_SkipSpace(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
	{
		p++;
	}
	return p;
}

/* 解析数字(整数/小数), 不用 strtod, 兼容 microlib */
static uint8_t AT_ParseNumber(const char **pp, AT_Value *val)
{
	const char *p = *pp;
	uint8_t neg = 0;
	uint8_t is_float = 0;
	int32_t ipart = 0;
	double frac = 0.0;
	double scale = 1.0;

	if (*p == '-')
	{
		neg = 1;
		p++;
	}
	if (*p < '0' || *p > '9')
	{
		return 0;
	}

	while (*p >= '0' && *p <= '9')
	{
		ipart = ipart * 10 + (*p - '0');
		p++;
	}
	if (*p == '.')
	{
		is_float = 1;
		p++;
		while (*p >= '0' && *p <= '9')
		{
			scale *= 10.0;
			frac = frac * 10.0 + (double)(*p - '0');
			p++;
		}
	}
	/* 跳过科学计数法尾巴 */
	if (*p == 'e' || *p == 'E')
	{
		p++;
		if (*p == '+' || *p == '-')
		{
			p++;
		}
		while (*p >= '0' && *p <= '9')
		{
			p++;
		}
	}

	if (is_float)
	{
		val->type = AT_VAL_FLOAT;
		val->f = (double)ipart + frac / scale;
		if (neg)
		{
			val->f = -val->f;
		}
		val->i = (int32_t)val->f;
	}
	else
	{
		val->type = AT_VAL_INT;
		val->i = neg ? -ipart : ipart;
		val->f = (double)val->i;
	}

	*pp = p;
	return 1;
}

/* 解析字符串(支持 \" 之类简单转义) */
static uint8_t AT_ParseStr(const char **pp, char *out, uint16_t max)
{
	const char *p = *pp;
	uint16_t n = 0;
	char c;

	if (*p != '"')
	{
		return 0;
	}
	p++;

	while (*p != '\0' && *p != '"')
	{
		c = *p++;
		if (c == '\\' && *p != '\0')
		{
			c = *p++;
		}
		if (n < (uint16_t)(max - 1U))
		{
			out[n++] = c;
		}
	}
	if (*p == '"')
	{
		p++;
	}
	out[n] = '\0';

	*pp = p;
	return 1;
}

/* 跳过一个 {...} 对象: p指向'{', 返回'}'之后的位置 */
static const char *AT_SkipObject(const char *p)
{
	uint16_t depth = 0;

	if (*p != '{')
	{
		return p;
	}

	while (*p != '\0')
	{
		if (*p == '"')
		{
			p++;
			while (*p != '\0' && *p != '"')
			{
				if (*p == '\\' && p[1] != '\0')
				{
					p++;
				}
				p++;
			}
			if (*p == '"')
			{
				p++;
			}
			continue;
		}
		if (*p == '{')
		{
			depth++;
		}
		else if (*p == '}')
		{
			depth--;
			p++;
			if (depth == 0U)
			{
				return p;
			}
			continue;
		}
		p++;
	}
	return p;
}

/* 解析一个标量值: 数字 / 字符串 / true / false / null */
static uint8_t AT_ParseScalar(const char **pp, AT_Value *val)
{
	const char *p = AT_SkipSpace(*pp);

	*pp = p;

	if (*p == '"')
	{
		val->type = AT_VAL_STR;
		return AT_ParseStr(pp, val->s, (uint16_t)sizeof(val->s));
	}
	if (strncmp(p, "true", 4) == 0)
	{
		val->type = AT_VAL_BOOL;
		val->i = 1;
		val->f = 1.0;
		*pp = p + 4;
		return 1;
	}
	if (strncmp(p, "false", 5) == 0)
	{
		val->type = AT_VAL_BOOL;
		val->i = 0;
		val->f = 0.0;
		*pp = p + 5;
		return 1;
	}
	if (strncmp(p, "null", 4) == 0)
	{
		val->type = AT_VAL_INT;
		val->i = 0;
		val->f = 0.0;
		*pp = p + 4;
		return 1;
	}
	return AT_ParseNumber(pp, val);
}

/* 解析属性值: 标量, 或 {"value":xx} 这种对象 */
static uint8_t AT_ParseValue(const char **pp, AT_Value *val)
{
	const char *p = AT_SkipSpace(*pp);
	const char *obj_end;
	const char *q;

	if (*p == '{')
	{
		obj_end = AT_SkipObject(p);
		q = strstr(p, "\"value\"");
		if (q != NULL && q < obj_end)
		{
			q += 7;
			q = AT_SkipSpace(q);
			if (*q == ':')
			{
				q++;
			}
			q = AT_SkipSpace(q);
			if (AT_ParseScalar(&q, val))
			{
				*pp = obj_end;
				return 1;
			}
		}
		*pp = obj_end;
		return 0;
	}

	return AT_ParseScalar(pp, val);
}

/* 按照映射表把属性值赋给对应变量 */
static void AT_ApplyRecv(const char *name, const AT_Value *val)
{
	uint8_t i;
	uint8_t found = 0;
	uint16_t n;

	for (i = 0; i < (uint8_t)(sizeof(at_recv_table) / sizeof(at_recv_table[0])); i++)
	{
		if (strcmp(at_recv_table[i].name, name) != 0)
		{
			continue;
		}

		found = 1;

		switch (at_recv_table[i].type)
		{
		case AT_VAL_INT:
			if (val->type == AT_VAL_BOOL)
			{
				*(int32_t *)at_recv_table[i].dst = (int32_t)val->i;
			}
			else if (val->type == AT_VAL_FLOAT)
			{
				*(int32_t *)at_recv_table[i].dst = (int32_t)val->f;
			}
			else
			{
				*(int32_t *)at_recv_table[i].dst = val->i;
			}
			AT_Log("[AT] recv %s = %ld\r\n", name,
			       (long)(*(int32_t *)at_recv_table[i].dst));
			break;

		case AT_VAL_FLOAT:
			if (val->type != AT_VAL_STR)
			{
				*(float *)at_recv_table[i].dst = (float)val->f;
				AT_LogFloat("[AT] recv ", name,
				            (double)(*(float *)at_recv_table[i].dst));
			}
			break;

		case AT_VAL_BOOL:
			if (val->type == AT_VAL_STR)
			{
				*(uint8_t *)at_recv_table[i].dst =
					(uint8_t)((val->s[0] == '1' || val->s[0] == 't') ? 1 : 0);
			}
			else
			{
				*(uint8_t *)at_recv_table[i].dst = (uint8_t)((val->i != 0) ? 1 : 0);
			}
			AT_Log("[AT] recv %s = %u\r\n", name,
			       (unsigned)(*(uint8_t *)at_recv_table[i].dst));
			break;

		case AT_VAL_STR:
		{
			uint16_t k = 0;

			n = at_recv_table[i].size;
			if (n > 0U && val->type == AT_VAL_STR)
			{
				while (val->s[k] != '\0' && k < (uint16_t)(n - 1U))
				{
					((char *)at_recv_table[i].dst)[k] = val->s[k];
					k++;
				}
				((char *)at_recv_table[i].dst)[k] = '\0';
				AT_Log("[AT] recv %s = \"%s\"\r\n", name,
				       (char *)at_recv_table[i].dst);
			}
			break;
		}

		default:
			break;
		}

		break;          /* 一个属性只赋一次值 */
	}

	if (!found)
	{
		AT_Log("[AT] recv unknown property: %s\r\n", name);
	}

	/* 记录最近一次属性名, 并置"有新下发"标志 */
	n = 0;
	while (name[n] != '\0' && n < (uint16_t)(AT_RECV_NAME_LEN - 1U))
	{
		g_recv_name[n] = name[n];
		n++;
	}
	g_recv_name[n] = '\0';
	g_recv_updated = 1;

	AT_OnRecvProperty(name, val);
}

/* 解析 params 对象, 逐个属性判断并赋值 */
static void AT_ParseParams(const char *json)
{
	const char *p;
	char name[AT_RECV_NAME_LEN];
	AT_Value val;

	p = strstr(json, "\"params\"");
	if (p == NULL)
	{
		return;
	}
	p = strchr(p, '{');             /* params 对象开始 */
	if (p == NULL)
	{
		return;
	}
	p++;

	for (;;)
	{
		p = AT_SkipSpace(p);
		if (*p == '\0' || *p == '}')
		{
			break;
		}
		if (*p == ',')
		{
			p++;
			continue;
		}
		if (*p != '"')
		{
			break;
		}
		if (!AT_ParseStr(&p, name, (uint16_t)sizeof(name)))
		{
			break;
		}

		p = AT_SkipSpace(p);
		if (*p != ':')
		{
			break;
		}
		p++;
		p = AT_SkipSpace(p);

		memset(&val, 0, sizeof(val));
		if (!AT_ParseValue(&p, &val))
		{
			AT_Log("[AT] recv parse fail: %s\r\n", name);
			break;
		}

		AT_ApplyRecv(name, &val);
	}
}

/* 解析 +MSUB: "topic",<len> byte,<payload> */
static void AT_ParseDownlink(const char *urc)
{
	const char *p;
	const char *q;
	const char *end;
	uint16_t n;

	p = strstr(urc, "+MSUB:");
	if (p == NULL)
	{
		return;
	}
	p += 6;

	q = strchr(p, '"');             /* topic 起始 */
	if (q == NULL)
	{
		return;
	}
	q++;
	end = strchr(q, '"');           /* topic 结束 */
	if (end == NULL)
	{
		return;
	}

	q = strstr(end, " byte,");      /* 长度字段之后就是payload */
	if (q == NULL)
	{
		return;
	}
	q += 6;

	n = (uint16_t)strlen(q);
	if (n >= AT_URC_BUF_SIZE)
	{
		n = (uint16_t)(AT_URC_BUF_SIZE - 1U);
	}
	memcpy(at_downlink, q, n);
	at_downlink[n] = '\0';

	/* 去掉尾部换行 */
	while (n > 0 && (at_downlink[n - 1U] == '\r' || at_downlink[n - 1U] == '\n'))
	{
		n--;
		at_downlink[n] = '\0';
	}
	at_downlink_len = n;

	AT_Log("[AT] downlink(%u): ", (unsigned)n);
	AT_LogLong("", at_downlink);

	/* 判断平台下发的是哪个属性, 并赋值给对应变量 */
	AT_ParseParams(at_downlink);

#if (AT_AUTO_REPLY_SET != 0)
	/* 先记下消息id, 由 AT_Cloud_Task() 延时发送回复
	 * (立即发送时部分模组会掉线, 见 AT_REPLY_DELAY_MS) */
	if (AT_ParseMsgId(at_downlink, at_reply_id, (uint16_t)sizeof(at_reply_id)))
	{
		at_reply_pending = 1;
		at_reply_tick = HAL_GetTick();
	}
#endif
}

/* 处理模组主动上报(平台下发/掉线通知) */
void AT_Task(void)
{
	static char urc[AT_URC_BUF_SIZE];
	uint16_t n;

	n = USART2_ReadChunk((uint8_t *)urc, (uint16_t)(AT_URC_BUF_SIZE - 1U));
	if (n == 0)
	{
		return;
	}
	urc[n] = '\0';

	if (strstr(urc, "+MSUB:") != NULL)
	{
		AT_ParseDownlink(urc);
	}
	else
	{
		AT_LogLong("[AT] <== ", urc);
	}

	if (strstr(urc, "+MQTTDISCONNECTED") != NULL ||
	    strstr(urc, "CONNACK FAIL") != NULL ||
	    strstr(urc, "\r\nCLOSED") != NULL ||
	    (strncmp(urc, "CLOSED", 6) == 0) ||
	    strstr(urc, "PDP DEACT") != NULL)
	{
		/* 模块上报: MQTT被断开 / 网络掉线
		 * 立即标记未连接, 下一圈AT_Cloud_Task会尽快重连 */
		if (at_connected)
		{
			AT_Log("[AT] connection lost (%s), will reconnect\r\n",
			       (strstr(urc, "CLOSED") != NULL) ? "CLOSED" : "NETWORK");
		}
		at_connected = 0;
	}
}

/* ==================== 集成任务 ==================== */

/* 放进主循环:
 *   未连接 -> 自动连接(带重连间隔)
 *   已连接 -> 周期上传 + 处理下发 */
void AT_Cloud_Task(void)
{
	uint32_t now;

	AT_Task();

	now = HAL_GetTick();

#if (AT_AUTO_REPLY_SET != 0)
	/* 延时回复"属性设置"(收到后等 AT_REPLY_DELAY_MS 再发,
	 * 太急发送部分模组会把MQTT连接断掉) */
	if (at_reply_pending && ((now - at_reply_tick) >= AT_REPLY_DELAY_MS))
	{
		at_reply_pending = 0;
		AT_ReplyPropertySet();
	}
#endif

	if (!at_connected)
	{
		if (!at_conn_tried || (now - at_conn_tick) >= AT_RECONNECT_INTERVAL_MS)
		{
			at_conn_tried = 1;
			at_conn_tick  = now;

			if (AT_Connect() == AT_OK)
			{
				(void)AT_UploadParams();       /* 连上先传一次 */
			}
			else
			{
				at_conn_tick = HAL_GetTick();
				AT_Log("[AT] connect failed, retry after %u ms\r\n",
				       (unsigned)AT_RECONNECT_INTERVAL_MS);
			}
		}
		return;
	}

	if ((now - at_upload_tick) >= AT_UPLOAD_PERIOD_MS)
	{
		(void)AT_UploadParams();
	}
}

/* ==================== 初始化 / 查询 ==================== */

void AT_Init(void)
{
	at_connected   = 0;
	at_conn_tried  = 0;
	at_conn_tick   = 0;
	at_upload_tick = 0;
	at_msg_id      = 1;
	at_upload_cnt  = 0;
	at_pub_fail    = 0;
	at_resp_len    = 0;
	at_downlink_len = 0;
	at_downlink[0] = '\0';

#if (AT_AUTO_REPLY_SET != 0)
	at_reply_pending = 0;
	at_reply_tick = 0;
	at_reply_id[0] = '\0';
#endif

	AT_Param_Reset();

	USART2_Start();     /* 确保串口2 DMA+空闲中断接收已开启 */

	AT_Log("\r\n[AT] init: product=%s device=%s\r\n",
	       AT_ONENET_PRODUCT_ID, AT_ONENET_DEVICE_NAME);
}

const char *AT_GetDownlink(void)
{
	return at_downlink;
}

uint16_t AT_GetDownlinkLen(void)
{
	return at_downlink_len;
}

uint32_t AT_GetUploadCount(void)
{
	return at_upload_cnt;
}
