#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdcard.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard_spi";

#define SDlist "/sdcard"

sdmmc_card_t *card_host = NULL;

esp_err_t sdcard_init(const sdcard_config_t *config)
{
    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024 * 3,
    };

    // Configure SD card device on SPI
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = config->cs_pin;
    slot_config.host_id = config->host_id;

    ESP_LOGI(TAG, "Mounting SD card via SPI (Host=%d, CS=%d)", config->host_id, config->cs_pin);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    esp_err_t ret = esp_vfs_fat_sdspi_mount(SDlist, &host, &slot_config, &mount_config, &card_host);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    if (card_host != NULL) {
        sdmmc_card_print_info(stdout, card_host);
        ESP_LOGI(TAG, "SD card mounted successfully");
        return ESP_OK;
    }

    return ESP_FAIL;
}
