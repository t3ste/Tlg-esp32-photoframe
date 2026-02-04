set(srcs "")
if(CONFIG_EP_DRIVER_SEEEDSTUDIO_T133A01)
    list(APPEND srcs "src/driver_t133a01.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    REQUIRES driver esp_driver_gpio esp_timer driver esp_driver_spi epaper
)
