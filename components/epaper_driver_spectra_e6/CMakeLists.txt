set(srcs "")
if(CONFIG_EP_DRIVER_SPECTRA_E6)
    list(APPEND srcs "src/driver_spectra_e6.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    REQUIRES driver esp_driver_gpio esp_timer driver esp_driver_spi epaper
)
