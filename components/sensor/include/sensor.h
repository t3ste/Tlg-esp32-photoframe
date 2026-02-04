#ifndef SENSOR_H
#define SENSOR_H

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the sensor
 * @param i2c_bus I2C master bus handle
 * @return ESP_OK on success
 */
esp_err_t sensor_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Read temperature and humidity
 * @param temperature Pointer to store temperature (Celsius)
 * @param humidity Pointer to store humidity (%)
 * @return ESP_OK on success
 */
esp_err_t sensor_read(float *temperature, float *humidity);

/**
 * @brief Put sensor to sleep
 * @return ESP_OK on success
 */
esp_err_t sensor_sleep(void);

/**
 * @brief Wake up sensor
 * @return ESP_OK on success
 */
esp_err_t sensor_wakeup(void);

/**
 * @brief Check if sensor is available
 * @return true if available
 */
bool sensor_is_available(void);

#ifdef __cplusplus
}
#endif

#endif  // SENSOR_H
