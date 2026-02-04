set(srcs "")

if(CONFIG_SDCARD_DRIVER_SDIO)
    list(APPEND srcs "src/sdcard_sdio.c")
endif()

if(CONFIG_SDCARD_DRIVER_SPI)
    list(APPEND srcs "src/sdcard_spi.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio driver fatfs sdmmc
)