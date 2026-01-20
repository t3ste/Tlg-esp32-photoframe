idf_component_register(
  SRCS "shtc3_sensor.c"
  REQUIRES driver esp_driver_i2c i2c_bsp
  INCLUDE_DIRS "./")
