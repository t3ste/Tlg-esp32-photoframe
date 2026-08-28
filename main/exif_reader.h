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

/**
 * @brief Reads the capture-date sidecar (`<name>.capture.json`) process-cli
 * writes alongside a Storage/Auto-Rotate album image - the only way to
 * recover a photo's EXIF capture date once it's been rendered to a
 * display-ready PNG/EPDGZ/BMP, which never carries EXIF, and once the
 * original JPEG is gone (per this project's SD-card convention, it generally
 * never reaches the device at all for this ingestion path). See
 * process-cli/capture-date.js for the writer and exact schema.
 *
 * @param anchor_path Path to the currently-displayed image - any of the
 * three shapes a rendered photo can take (bare "<name>.<ext>",
 * "<name>.fit.<ext>", or "crop/<name>.cover.<ext>") resolves to the same
 * sidecar, shared across all of a source photo's rendered variants.
 * @param out Filled with the sidecar's stored "YYYY-MM-DD HH:MM" string
 * verbatim (already formatted by process-cli to match this codebase's own
 * convention - no date parsing needed here), NUL-terminated, on success.
 * @param out_len Size of `out`, e.g. via sizeof().
 * @return true if a sidecar was found and `out` was filled; false if it
 * doesn't exist, doesn't parse, or has no capture_date field - callers
 * should treat false as "nothing to show", not an error (most photos have no
 * camera EXIF, or weren't processed by process-cli at all).
 */
bool capture_date_sidecar_read(const char *anchor_path, char *out, size_t out_len);

#endif
