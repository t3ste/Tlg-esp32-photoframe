#ifndef EXIF_READER_H
#define EXIF_READER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Reads the EXIF "DateTimeOriginal" tag (0x9003, the camera's capture
 * timestamp - distinct from the file's own mtime or the "ModifyDate" tag in
 * IFD0) from a JPEG file, if present.
 *
 * Only JPEG (APP1 "Exif\0\0" segment containing a TIFF structure) is
 * supported - there is no PNG equivalent in this codebase's pipeline, since
 * every PNG this firmware produces/consumes is either processed for e-paper
 * display (metadata already stripped) or a webapp-uploaded album image
 * (processed client-side in the browser before it ever reaches the device -
 * no original JPEG bytes are available server-side for that path).
 *
 * @param path Path to a JPEG file, as originally received (not yet processed
 * for e-paper display - processing may strip metadata).
 * @param out Filled with "YYYY-MM-DD HH:MM" (seconds dropped - not
 * meaningful for a caption line) on success, NUL-terminated. Left as an
 * empty string on failure.
 * @param out_len Size of `out`, e.g. via sizeof().
 * @return true if found and `out` was filled; false if the file isn't a
 * JPEG, has no Exif APP1 segment, or has no DateTimeOriginal tag - callers
 * should treat false as "nothing to show", not an error.
 */
bool exif_reader_get_datetime_original(const char *path, char *out, size_t out_len);

#endif
