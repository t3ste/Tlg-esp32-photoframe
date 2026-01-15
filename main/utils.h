#ifndef UTILS_H
#define UTILS_H

#include "esp_err.h"

// Fetch image from URL, convert to BMP, and save to Downloads album
// Returns ESP_OK on success, error code on failure
// saved_bmp_path will contain the path to the saved BMP file
esp_err_t fetch_and_save_image_from_url(const char *url, char *saved_bmp_path, size_t path_size);

// Trigger image rotation based on configured rotation mode
// Handles both URL and SD card rotation modes
// Returns ESP_OK on success, error code on failure
esp_err_t trigger_image_rotation(void);

#endif
