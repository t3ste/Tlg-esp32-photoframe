#ifndef PNG_DECODER_H
#define PNG_DECODER_H

#include "esp_err.h"

/**
 * @brief Decode PNG file to BMP file
 *
 * @param png_path Path to input PNG file
 * @param bmp_path Path to output BMP file
 * @return esp_err_t ESP_OK on success
 */
esp_err_t png_decode_to_bmp(const char *png_path, const char *bmp_path);

#endif  // PNG_DECODER_H
