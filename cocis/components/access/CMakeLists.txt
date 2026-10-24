idf_component_register(
    SRCS "rfid.c" "door.c"
    INCLUDE_DIRS "include"
    REQUIRES common esp_driver_spi esp_driver_gpio esp_driver_ledc esp_timer
)