#ifndef HA_INTEGRATION_H
#define HA_INTEGRATION_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Post battery status to Home Assistant
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t ha_post_battery_status(void);

/**
 * @brief Check if Home Assistant URL is configured
 *
 * @return true if HA URL is configured, false otherwise
 */
bool ha_is_configured(void);

#endif
