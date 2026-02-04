set(srcs "")
if(CONFIG_SENSOR_DRIVER_SHTC3)
    list(APPEND srcs "shtc3_sensor.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    REQUIRES driver esp_driver_i2c sensor
)
