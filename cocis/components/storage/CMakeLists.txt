idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES common driver fatfs sdmmc freertos
)