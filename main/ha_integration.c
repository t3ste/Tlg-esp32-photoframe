#include "ha_integration.h"

#include <string.h>

#include "axp_prot.h"
#include "config_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "ha_integration";

esp_err_t ha_post_battery_status(void)
{
    const char *ha_url = config_manager_get_ha_url();

    // Check if HA URL is configured
    if (ha_url == NULL || strlen(ha_url) == 0) {
        ESP_LOGD(TAG, "HA URL not configured, skipping battery post");
        return ESP_OK;  // Not an error, just not configured
    }

    // Get battery data
    int battery_percent = axp_get_battery_percent();
    int battery_voltage = axp_get_battery_voltage();

    if (battery_percent < 0 || battery_voltage < 0) {
        ESP_LOGW(TAG, "Failed to read battery data");
        return ESP_FAIL;
    }

    // Build the API endpoint URL
    char url[512];
    snprintf(url, sizeof(url), "%s/api/esp32_photoframe/battery", ha_url);

    // Build JSON payload
    char json_payload[256];
    snprintf(json_payload, sizeof(json_payload), "{\"battery_level\":%d,\"battery_voltage\":%d}",
             battery_percent, battery_voltage);

    ESP_LOGI(TAG, "Posting battery status to HA: %s", json_payload);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .user_agent = "ESP32-PhotoFrame/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // Set headers and data
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_payload, strlen(json_payload));

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "HA returned HTTP %d", status_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Battery status posted to HA successfully");
    return ESP_OK;
}

bool ha_is_configured(void)
{
    const char *ha_url = config_manager_get_ha_url();
    return (ha_url != NULL && strlen(ha_url) > 0);
}
