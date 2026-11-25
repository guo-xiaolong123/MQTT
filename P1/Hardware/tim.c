#include "tim.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "TIM";

//定时器句柄
static gptimer_handle_t tim0_handle = NULL;
static gptimer_handle_t tim1_handle = NULL;
static gptimer_handle_t tim2_handle = NULL;
static gptimer_handle_t tim3_handle = NULL;

//中断声明
static bool TIM0_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx);
static bool TIM1_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx);
static bool TIM2_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx);
static bool TIM3_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx);

volatile uint8_t LED_flag=0;



//初始化4个定时器，新建-设报警值-注册中断-使能中断
void tim_init(void)
{
    /* ============================ 定时器 0 ============================ */
    gptimer_config_t config0 = {0};                  //先清零
    config0.clk_src       = GPTIMER_CLK_SRC_DEFAULT; //默认时钟源
    config0.direction     = GPTIMER_COUNT_UP;        //计数方向
    config0.resolution_hz = 1000000;                 //计数器分辨率(工作频率)为 1MHz, 每个计数 tick 的步长为 (1 / 1000000) 秒 = 1 us
    config0.intr_priority = 0;                       //中断优先级自动分配
    config0.flags.intr_shared = 0;                   //中断号不共享

    gptimer_alarm_config_t alarm0 = {0};
    alarm0.alarm_count = 1000000;                    //1秒
    alarm0.reload_count = 0;                         //重载计数值到0
    alarm0.flags.auto_reload_on_alarm = 1;           //中断触发后自动重载计数值

    gptimer_event_callbacks_t cbs0 = {0};
    cbs0.on_alarm = TIM0_ISR;                        //关联中断函数

    ESP_ERROR_CHECK(gptimer_new_timer(&config0, &tim0_handle));                   //申请定时器，申请失败会打印错误信息并重启
    ESP_ERROR_CHECK(gptimer_set_alarm_action(tim0_handle, &alarm0));              //检查是否设置成功
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(tim0_handle, &cbs0, NULL));  //是否关联成功
    ESP_ERROR_CHECK(gptimer_enable(tim0_handle));                                 //使能定时器中断

    /* ============================ 定时器 1 ============================ */
    gptimer_config_t config1 = {0};
    config1.clk_src       = GPTIMER_CLK_SRC_DEFAULT;
    config1.direction     = GPTIMER_COUNT_UP;
    config1.resolution_hz = 1000000;
    config1.intr_priority = 0;
    config1.flags.intr_shared = 0;

    gptimer_alarm_config_t alarm1 = {0};
    alarm1.alarm_count = 1000000;
    alarm1.reload_count = 0;
    alarm1.flags.auto_reload_on_alarm = 1;

    gptimer_event_callbacks_t cbs1 = {0};
    cbs1.on_alarm = TIM1_ISR;

    ESP_ERROR_CHECK(gptimer_new_timer(&config1, &tim1_handle));
    ESP_ERROR_CHECK(gptimer_set_alarm_action(tim1_handle, &alarm1));
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(tim1_handle, &cbs1, NULL));
    ESP_ERROR_CHECK(gptimer_enable(tim1_handle));

    /* ============================ 定时器 2 ============================ */
    gptimer_config_t config2 = {0};
    config2.clk_src       = GPTIMER_CLK_SRC_DEFAULT;
    config2.direction     = GPTIMER_COUNT_UP;
    config2.resolution_hz = 1000000;
    config2.intr_priority = 0;
    config2.flags.intr_shared = 0;

    gptimer_alarm_config_t alarm2 = {0};
    alarm2.alarm_count = 1000000;
    alarm2.reload_count = 0;
    alarm2.flags.auto_reload_on_alarm = 1;

    gptimer_event_callbacks_t cbs2 = {0};
    cbs2.on_alarm = TIM2_ISR;

    ESP_ERROR_CHECK(gptimer_new_timer(&config2, &tim2_handle));
    ESP_ERROR_CHECK(gptimer_set_alarm_action(tim2_handle, &alarm2));
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(tim2_handle, &cbs2, NULL));
    ESP_ERROR_CHECK(gptimer_enable(tim2_handle));

    /* ============================ 定时器 3 ============================ */
    gptimer_config_t config3 = {0};
    config3.clk_src       = GPTIMER_CLK_SRC_DEFAULT;
    config3.direction     = GPTIMER_COUNT_UP;
    config3.resolution_hz = 1000000;
    config3.intr_priority = 0;
    config3.flags.intr_shared = 0;

    gptimer_alarm_config_t alarm3 = {0};
    alarm3.alarm_count = 1000000;
    alarm3.reload_count = 0;
    alarm3.flags.auto_reload_on_alarm = 1;

    gptimer_event_callbacks_t cbs3 = {0};
    cbs3.on_alarm = TIM3_ISR;

    ESP_ERROR_CHECK(gptimer_new_timer(&config3, &tim3_handle));
    ESP_ERROR_CHECK(gptimer_set_alarm_action(tim3_handle, &alarm3));
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(tim3_handle, &cbs3, NULL));
    ESP_ERROR_CHECK(gptimer_enable(tim3_handle));

    ESP_LOGI(TAG, "4 timers init ok, period = 1s");
}

//按照编号选取定时器句柄
static gptimer_handle_t tim_get_handle(tim_id_t id)
{
    switch (id) 
    {
        case TIM_0: return tim0_handle;
        case TIM_1: return tim1_handle;
        case TIM_2: return tim2_handle;
        case TIM_3: return tim3_handle;
        default:    return NULL;
    }
}

//开启指定定时器中断
void tim_start(tim_id_t id)
{
    gptimer_handle_t handle = tim_get_handle(id);
    if (handle == NULL) 
    {
        ESP_LOGE(TAG, "tim_start(): invalid id %d", (int)id);
        return;
    }

    gptimer_set_raw_count(handle, 0);       //如果是关掉后重启，保证从0开始重新计数

    esp_err_t err = gptimer_start(handle);
    if (err == ESP_ERR_INVALID_STATE) 
    {
        return;                             //定时器已经启动了
    }
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "TIM%d gptimer_start() error: %s", (int)id, esp_err_to_name(err));  //打印错误信息
        return;
    }
    ESP_LOGI(TAG, "TIM%d started", (int)id);              //成功启动
}

//关闭指定定时器中断
void tim_stop(tim_id_t id)
{
    gptimer_handle_t handle = tim_get_handle(id);
    if (handle == NULL) {
        ESP_LOGE(TAG, "tim_stop(): invalid id %d", (int)id);
        return;
    }

    esp_err_t err = gptimer_stop(handle);
    if (err == ESP_ERR_INVALID_STATE) {
        return;                            
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TIM%d gptimer_stop() error: %s", (int)id, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "TIM%d stopped", (int)id);
}

//定时器0中断
static bool IRAM_ATTR TIM0_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    (void)timer;   //无效参数
    (void)edata;   //无效参数
    (void)user_ctx;//无效参数
    //定时器0逻辑

    LED_flag=!LED_flag;
   

    return false;
}

//定时器1中断
static bool IRAM_ATTR TIM1_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;
    //定时器1逻辑
   

    return false;
}

//定时器2中断
static bool IRAM_ATTR TIM2_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;
    //定时器2逻辑



   

    return false;
}

//定时器3中断
static bool IRAM_ATTR TIM3_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;
    //定时器3逻辑

    return false;
}
