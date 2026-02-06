// filename: GUI_RawBuffer.h
#ifndef __GUI_RAWBUFFER_H
#define __GUI_RAWBUFFER_H

#include <stdint.h>

#include "GUI_Paint.h"

/**
 * @brief Display an RGB buffer directly on the e-paper display
 *
 * This function takes an already-processed RGB buffer (with colors matching
 * the 6-color palette) and paints it directly to the display buffer.
 * This is more efficient than encoding to PNG and decoding again.
 *
 * @param rgb_buffer RGB888 buffer (3 bytes per pixel, already dithered to palette)
 * @param width Image width
 * @param height Image height
 * @param Xstart Starting X coordinate
 * @param Ystart Starting Y coordinate
 * @return 0 on success, 1 on error
 */
UBYTE GUI_DisplayRGBBuffer_6Color(const uint8_t *rgb_buffer, int width, int height, UWORD Xstart,
                                  UWORD Ystart);

#endif
