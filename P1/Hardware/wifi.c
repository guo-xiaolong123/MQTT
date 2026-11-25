#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_sntp.h"


static volatile bool s_connected   = false;     // 是否已连上并拿到 IP
static volatile bool s_time_synced = false;     // 是否已对时成功

// ======================================================================
// WiFi / IP 事件回调
//   WIFI_EVENT_STA_START        : 驱动启动好了, 主动去连路由器
//   WIFI_EVENT_STA_CONNECTED    : 连上路由器(还没拿到 IP)
//   WIFI_EVENT_STA_DISCONNECTED : 断开(密码错/信号差), 自动重连
//   IP_EVENT_STA_GOT_IP         : DHCP 拿到 IP, 真正能上网了
// ====================================================================
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf("[WIFI] start, connecting to \"%s\" ...\n", WIFI_SSID);
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        printf("[WIFI] connected to AP, waiting for IP ...\n");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        printf("[WIFI] disconnected, retry ...\n");
        esp_wifi_connect();                     // 断线自动重连
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_connected = true;
        printf("[WIFI] got ip: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    }
}

// ======================================================================
// 初始化 WiFi 并开始连接 (STA 模式)
// ====================================================================
void wifi_init_sta(void)
{
    // 1. NVS: WiFi 驱动要把校准数据存到 NVS 里, 必须初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 网络接口 + 默认事件循环
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. 初始化 WiFi 驱动
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. 注册事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 5. 设为 STA 模式, 填入 SSID 和密码
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // 6. 启动 WiFi, 启动后会触发 WIFI_EVENT_STA_START 自动去连接
    ESP_ERROR_CHECK(esp_wifi_start());

    // 注意: 这里没有创建任务, esp_wifi_start() 内部会创建 wifi 驱动任务
    //       (日志里打印的 prio:23, stack:6656, core=0 就是它)
}
// ======================================================================
// 阻塞等待 WiFi 连上(带超时), 连上返回 true
// ====================================================================
bool wifi_wait_connected(uint32_t timeout_ms)
{
    uint32_t waited = 0;

    while (!s_connected) {
        if (timeout_ms != 0 && waited >= timeout_ms) {
            printf("\n[WIFI] connect timeout\n");
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

// ======================================================================
// 查询 WiFi 是否已连上并拿到 IP
// ====================================================================
bool wifi_is_connected(void)
{
    return s_connected;
}

// ======================================================================
// SNTP 对时完成时的回调
// ====================================================================
static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    printf("[TIME] sntp sync ok\n");
}

// ======================================================================
// 启动 SNTP 网络对时
//   时区设为东八区(CST-8), 这样打印出来就是北京时间
// ====================================================================
void time_sync_start(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");       // 阿里云 NTP 服务器
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    printf("[TIME] sntp start, server: ntp.aliyun.com\n");
}

// ======================================================================
// 查询是否已经对时成功
// ====================================================================
bool time_is_synced(void)
{
    if (!s_time_synced && esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        s_time_synced = true;
    }
    return s_time_synced;
}

