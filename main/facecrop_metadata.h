#ifndef FACECROP_METADATA_H
#define FACECROP_METADATA_H

#include <stdbool.h>

#include "image_processor.h"

/**
 * @brief Reads a "<name>.facecrop.json" sidecar's recommended_crop field
 *
 * See docs/FACE_CROP.md for the full schema - process-cli's --detect-faces
 * writes this file next to an original image whenever it ran face
 * detection against it. The crop rectangle is in the source image's own
 * (EXIF-corrected, non-resized) pixel space, matching what
 * image_processor_render_variant()'s `crop` parameter expects directly.
 *
 * @param original_path Path to the original image (e.g. "<dir>/name.jpg") -
 *   the metadata path is derived from this by replacing its extension with
 *   ".facecrop.json".
 * @param out Filled in only when this returns true.
 * @return false if the sidecar doesn't exist, isn't valid JSON, or has no
 *   valid recommended_crop - callers should treat that as "no crop
 *   metadata available" (fall back to an uncropped/center-crop render),
 *   not as an error worth surfacing loudly.
 */
bool facecrop_read_recommended_crop(const char *original_path, image_crop_rect_t *out);

#endif
