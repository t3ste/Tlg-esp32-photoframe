set(srcs "")
if(CONFIG_SENSOR_DRIVER_SHT40)
    list(APPEND srcs "src/sht40_sensor.c")
endif()

idf_component_register(
    SRCS "${srcs}"
    REQUIRES driver esp_driver_i2c sensor
)
