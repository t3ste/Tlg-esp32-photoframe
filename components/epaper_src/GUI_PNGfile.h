// filename: GUI_PNGfile.h
#ifndef __GUI_PNGFILE_H
#define __GUI_PNGFILE_H

#include "GUI_Paint.h"

/**
 * @brief Read PNG file and display it on the e-paper display
 *
 * @param path Path to the PNG file
 * @param Xstart Starting X coordinate
 * @param Ystart Starting Y coordinate
 * @return 0 on success, 1 on error
 */
UBYTE GUI_ReadPng_RGB_6Color(const char *path, UWORD Xstart, UWORD Ystart);

#endif
