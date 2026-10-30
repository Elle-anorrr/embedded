idf_component_register(SRCS "cli.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common esp_driver_uart nvs_flash mbedtls esp_hw_support)