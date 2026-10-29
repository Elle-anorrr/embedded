idf_component_register(
    SRCS "comms.c" "common.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos log
    PRIV_REQUIRES esp_event esp_netif esp_wifi json mqtt
)