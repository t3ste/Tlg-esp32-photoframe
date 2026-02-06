// filename: GUI_RawBuffer.c
#include "GUI_RawBuffer.h"

#include <esp_log.h>

static const char *TAG = "GUI_RawBuffer";

UBYTE GUI_DisplayRGBBuffer_6Color(const uint8_t *rgb_buffer, int width, int height, UWORD Xstart,
                                  UWORD Ystart)
{
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "NULL rgb_buffer");
        return 1;
    }

    ESP_LOGI(TAG, "Displaying RGB buffer: %dx%d at (%d,%d)", width, height, Xstart, Ystart);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if ((Xstart + x) >= Paint.Width || (Ystart + y) >= Paint.Height) {
                continue;
            }

            int offset = (y * width + x) * 3;
            uint8_t r = rgb_buffer[offset + 0];
            uint8_t g = rgb_buffer[offset + 1];
            uint8_t b = rgb_buffer[offset + 2];

            // Map RGB to 6-color palette index
            // The buffer should already be dithered to palette colors
            UBYTE color;
            if (r == 0 && g == 0 && b == 0) {
                color = 0;  // Black
            } else if (r == 255 && g == 255 && b == 255) {
                color = 1;  // White
            } else if (r == 255 && g == 255 && b == 0) {
                color = 2;  // Yellow
            } else if (r == 255 && g == 0 && b == 0) {
                color = 3;  // Red
            } else if (r == 0 && g == 0 && b == 255) {
                color = 5;  // Blue
            } else if (r == 0 && g == 255 && b == 0) {
                color = 6;  // Green
            } else {
                // Fallback: find closest color (shouldn't happen if properly dithered)
                color = 1;  // Default to white
            }

            Paint_SetPixel(Xstart + x, Ystart + y, color);
        }
    }

    ESP_LOGI(TAG, "RGB buffer displayed successfully");
    return 0;
}
