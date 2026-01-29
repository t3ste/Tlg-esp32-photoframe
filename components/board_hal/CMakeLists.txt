set(srcs "")
if(CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER)
    list(APPEND srcs "src/driver_waveshare_photopainter.c")
elseif(CONFIG_BOARD_DRIVER_XIAO_EE02)
    list(APPEND srcs "src/driver_xiao_ee02.c")
endif()



if(CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER)
    idf_component_register(
        SRCS "${srcs}"
        INCLUDE_DIRS "include"
        REQUIRES esp_timer driver esp_adc esp_driver_gpio epaper_port
        PRIV_REQUIRES axpPower shtc3_sensor pcf85063_rtc
    )
else()
     idf_component_register(
        SRCS "${srcs}"
        INCLUDE_DIRS "include"
        REQUIRES esp_timer driver esp_adc esp_driver_gpio epaper_port
    )
endif()
