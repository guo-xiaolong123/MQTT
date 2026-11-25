#ifndef __WIFI_H_
#define __WIFI_H_

#include <stdbool.h>
#include <stdint.h>

// ================== 在这里填你家路由器的信息 ==================
// 注意: ESP32-S3 只支持 2.4G 频段, 5G 的 WiFi 连不上。
#define WIFI_SSID   "hjhjhj123"          // WiFi 名称
#define WIFI_PASS   "hjhjhj123"        // WiFi 密码

// 初始化 WiFi 并开始连接(立即返回, 连接结果通过事件打印)
void wifi_init_sta(void);

// 阻塞等待连上 WiFi, 超时返回 false (timeout_ms 传 0 表示一直等)
bool wifi_wait_connected(uint32_t timeout_ms);

// 查询是否已经连上并且拿到 IP
bool wifi_is_connected(void);

// 启动 SNTP 网络对时(需要先联网成功)
void time_sync_start(void);

// 查询是否已经对时成功
bool time_is_synced(void);

#endif // __WIFI_H_
