idf_component_register(
    SRCS "led.c"
    "tim.c"
    "wifi.c"
    "MQTT.c"
    "dht22.c"
    "uart_cmd.c"
    "st7735.c"
    "display.c"           
    INCLUDE_DIRS "."      
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash lwip mqtt json      
)

# 这是可选的编译选项：
target_compile_options(${COMPONENT_LIB} PRIVATE -ffast-math -O3 -Wno-error=format -Wno-format)