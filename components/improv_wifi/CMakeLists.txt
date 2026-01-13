idf_component_register(
    SRCS "improv.cpp" "improv_serial.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_driver_uart
    PRIV_REQUIRES main
)
