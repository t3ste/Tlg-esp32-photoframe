#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Improv Wi-Fi Serial service
 * 
 * This will monitor UART0 (USB serial) for Improv protocol commands
 * and handle WiFi provisioning via serial connection.
 * 
 * @return ESP_OK on success
 */
esp_err_t improv_serial_init(void);

/**
 * @brief Start Improv Wi-Fi Serial service
 * 
 * Begins listening for Improv commands on serial port
 * 
 * @return ESP_OK on success
 */
esp_err_t improv_serial_start(void);

/**
 * @brief Stop Improv Wi-Fi Serial service
 * 
 * @return ESP_OK on success
 */
esp_err_t improv_serial_stop(void);

/**
 * @brief Check if Improv Serial is currently running
 * 
 * @return true if running, false otherwise
 */
bool improv_serial_is_running(void);

/**
 * @brief Set the redirect URL to return after successful provisioning
 * 
 * @param url The URL to redirect to (e.g., "http://photoframe.local")
 */
void improv_serial_set_redirect_url(const char *url);

#ifdef __cplusplus
}
#endif
