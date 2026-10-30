idf_component_register(
    SRCS "comms.c" "common.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos log driver nvs_flash
    PRIV_REQUIRES esp_event esp_netif esp_wifi mqtt
)