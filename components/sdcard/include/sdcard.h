#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_SDCARD_DRIVER_SDIO
/**
 * @brief SD card configuration for SDIO interface
 */
typedef struct {
    gpio_num_t clk_pin;
    gpio_num_t cmd_pin;
    gpio_num_t d0_pin;
    gpio_num_t d1_pin;
    gpio_num_t d2_pin;
    gpio_num_t d3_pin;
} sdcard_config_t;

/**
 * @brief Initialize SD card
 *
 * @param config Pointer to configuration structure
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t sdcard_init(const sdcard_config_t *config);
#endif

#ifdef CONFIG_SDCARD_DRIVER_SPI
/**
 * @brief SD card configuration for SPI interface
 */
typedef struct {
    int host_id;
    gpio_num_t cs_pin;
} sdcard_config_t;

/**
 * @brief Initialize SD card
 *
 * @param config Pointer to configuration structure
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t sdcard_init(const sdcard_config_t *config);
#endif

#ifdef __cplusplus
}
#endif