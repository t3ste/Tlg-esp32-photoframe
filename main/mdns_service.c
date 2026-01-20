#include "mdns_service.h"

#include <string.h>

#include "config_manager.h"
#include "esp_log.h"
#include "mdns.h"
#include "utils.h"

static const char *TAG = "mdns_service";

esp_err_t mdns_service_init(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    // Get device name and sanitize it for mDNS hostname
    const char *device_name = config_manager_get_device_name();
    char hostname[64];
    sanitize_hostname(device_name, hostname, sizeof(hostname));

    ESP_LOGI(TAG, "Device name: %s", device_name);
    ESP_LOGI(TAG, "mDNS hostname: %s", hostname);

    // Set hostname
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS hostname: %s", esp_err_to_name(err));
        return err;
    }

    // Set instance name (use the original device name for display)
    err = mdns_instance_name_set(device_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS instance name: %s", esp_err_to_name(err));
        return err;
    }

    // Add HTTP service
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add HTTP service: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS service started");
    ESP_LOGI(TAG, "Device accessible at: http://%s.local", hostname);

    return ESP_OK;
}

esp_err_t mdns_service_update_hostname(void)
{
    // Get device name and sanitize it for mDNS hostname
    const char *device_name = config_manager_get_device_name();
    char hostname[64];
    sanitize_hostname(device_name, hostname, sizeof(hostname));

    ESP_LOGI(TAG, "Updating mDNS hostname to: %s", hostname);

    // Free existing mDNS service to send goodbye packets
    mdns_free();

    // Reinitialize mDNS with new hostname
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reinitialize mDNS: %s", esp_err_to_name(err));
        return err;
    }

    // Set new hostname
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS hostname: %s", esp_err_to_name(err));
        return err;
    }

    // Set instance name (use the original device name for display)
    err = mdns_instance_name_set(device_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS instance name: %s", esp_err_to_name(err));
        return err;
    }

    // Re-add HTTP service
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add HTTP service: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "mDNS hostname updated successfully");
    ESP_LOGI(TAG, "Device now accessible at: http://%s.local", hostname);

    return ESP_OK;
}
