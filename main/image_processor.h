#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_manager.h"
#include "esp_err.h"

typedef enum {
    DITHER_FLOYD_STEINBERG,
    DITHER_STUCKI,
    DITHER_BURKES,
    DITHER_SIERRA
} dither_algorithm_t;

typedef enum {
    IMAGE_FORMAT_UNKNOWN,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_BMP,
    IMAGE_FORMAT_JPG,
    IMAGE_FORMAT_EPD_GZ
} image_format_t;

/**
 * @brief A crop rectangle in the source image's own pixel space (before any
 * resizing) - e.g. a face-crop metadata sidecar's "recommended_crop" (see
 * docs/FACE_CROP.md). Used by image_processor_render_variant() below.
 */
typedef struct {
    int x;
    int y;
    int w;
    int h;
} image_crop_rect_t;

/**
 * @brief Result structure for raw RGB buffer output (no PNG encoding)
 *
 * Only used by the Telegram-pairing/thumbnail helpers below, which need an
 * in-RAM buffer to composite/downsample - everything else in this project
 * uses the streaming, no-full-buffer path (image_processor_process_to_display
 * / image_processor_process_or_display_png).
 */
typedef struct {
    uint8_t *rgb_data;  // RGB888 buffer (caller must free with heap_caps_free)
    size_t rgb_size;    // Size of RGB data in bytes (width * height * 3)
    int width;          // Output image width
    int height;         // Output image height
} image_process_rgb_result_t;

esp_err_t image_processor_init(void);

/**
 * @brief Process image from file to file (legacy interface)
 *
 * This function reads from input_path, processes the image, and writes to output_path.
 * For SD-card systems, this is the preferred interface.
 */
esp_err_t image_processor_process(const char *input_path, const char *output_path,
                                  dither_algorithm_t dither_algorithm);

/**
 * @brief Process image from file to file, choosing the output format
 *
 * Same as image_processor_process(), but lets the caller request EPDGZ
 * instead of PNG (image_processor_process() is a thin wrapper always
 * requesting IMAGE_FORMAT_PNG). If EPDGZ is requested but its ~260 KB of
 * deflate state can't be allocated, this falls back to writing PNG instead -
 * @p output_path is expected to already carry the extension matching
 * @p out_format; on a PNG fallback, the file is written under the same path
 * with its extension replaced by ".png".
 *
 * @param out_format IMAGE_FORMAT_PNG or IMAGE_FORMAT_EPD_GZ - any other
 *   value is treated as IMAGE_FORMAT_PNG.
 * @param out_actual_format Optional (may be NULL) - set to the format that
 *   was actually written (differs from @p out_format only on the OOM
 *   fallback above), so the caller can adjust the path/extension it uses
 *   afterward.
 */
esp_err_t image_processor_process_fmt(const char *input_path, const char *output_path,
                                      dither_algorithm_t dither_algorithm, image_format_t out_format,
                                      image_format_t *out_actual_format);

/**
 * @brief Render a specific Cover or Fit variant of an image on demand
 *
 * Used by the firmware's on-device fallback when a wanted pre-rendered
 * Cover/Fit variant is missing (see docs/FACE_CROP.md) - decodes
 * @p input_path (JPG or PNG only - the oversized-document streaming
 * fallback tier is not supported here, since it can't apply a crop) and
 * renders it at a SPECIFIC scale mode regardless of the device's own
 * current setting, optionally pre-cropping to @p crop (in the source
 * image's own pixel space, before any resizing - e.g. a face-crop
 * metadata sidecar's recommended_crop) before the resize/dither pipeline
 * runs, so the crop is applied losslessly rather than as a second,
 * lossy crop on top of an already-resized image.
 *
 * Same format-selection/fallback contract as image_processor_process_fmt():
 * @p output_path must already carry the extension matching @p out_format,
 * falls back to PNG (renaming to ".png") if EPDGZ can't be allocated, and
 * reports the format actually written via @p out_actual_format.
 *
 * @param forced_scale_mode SCALE_MODE_COVER or SCALE_MODE_FIT (processing_settings.h)
 * @param crop Optional (may be NULL) - a crop rectangle in the source
 *   image's own pixel space. Bounds-clamped against the decoded image
 *   size; NULL (or an empty/invalid rect) renders the plain, uncropped
 *   source at @p forced_scale_mode.
 */
esp_err_t image_processor_render_variant(const char *input_path, const char *output_path,
                                         dither_algorithm_t dither_algorithm, image_format_t out_format,
                                         image_format_t *out_actual_format, int forced_scale_mode,
                                         const image_crop_rect_t *crop);

/**
 * @brief Process image from memory buffer and show it on the display
 *
 * This function takes raw image data (PNG or JPG), processes it, and streams
 * the result row by row straight into the display buffer, then refreshes the
 * panel. The full-resolution processed image is never materialized in RAM.
 *
 * @param input_data Raw image data (PNG or JPG format)
 * @param input_size Size of input data in bytes
 * @param format Image format of input data
 * @param dither_algorithm Dithering algorithm to use
 * @param pub What to publish on completion (current-image name, optional
 *            album snapshot and fallback; see display_publish_t); NULL for
 *            an anonymous buffer display
 * @return esp_err_t ESP_OK on success; ESP_ERR_NOT_FINISHED when displayed
 *         but the requested snapshot failed
 */
esp_err_t image_processor_process_to_display(const uint8_t *input_data, size_t input_size,
                                             image_format_t format,
                                             dither_algorithm_t dither_algorithm,
                                             const display_publish_t *pub);

/**
 * @brief Display a PNG file, processing it only when necessary
 *
 * A pre-processed PNG (native dimensions, every pixel a theoretical output
 * color) is validated and painted straight from the file in a single decode
 * with no RAM copy; anything else falls back to
 * image_processor_process_to_display. Preferred entry point for PNG display
 * requests. With release_source set, the file is unlinked as soon as an
 * in-RAM copy exists (for MemFS-backed sources that live in PSRAM).
 */
esp_err_t image_processor_process_or_display_png(const char *path,
                                                 dither_algorithm_t dither_algorithm,
                                                 const display_publish_t *pub, bool release_source);

esp_err_t image_processor_reload_palette(void);

/**
 * @brief Human-readable reason for the most recent processing failure
 *
 * Empty string when the last operation succeeded. Suitable for appending to
 * HTTP error responses.
 */
const char *image_processor_get_last_error(void);

image_format_t image_processor_detect_format(const char *input_path);

/**
 * @brief Whether a PNG file is already a fully processed, display-ready
 * image: native panel dimensions and every pixel one of the panel's
 * theoretical output colors. Used by the Telegram/pairing paths to decide
 * whether a file can be shown as-is or needs (re-)processing first.
 *
 * Checks against the fixed theoretical palette only, not a board's
 * calibrated/measured palette - a narrower check than
 * image_processor_process_or_display_png()'s internal equivalent, but
 * sufficient for its callers' purpose (avoid redundant reprocessing, not an
 * exact calibration match).
 */
bool image_processor_is_processed(const char *input_path);

/**
 * @brief Detect image format from buffer data
 */
image_format_t image_processor_detect_format_buffer(const uint8_t *data, size_t size);

/**
 * @brief Read just the pixel dimensions of an encoded image (PNG/JPG) without
 * decoding pixel data. Used to classify portrait vs. landscape cheaply before
 * committing to a full decode.
 */
/**
 * @brief Same as image_processor_peek_dimensions(), but reads the leading
 * bytes of a file itself rather than requiring the caller to already have a
 * buffer - a header-only peek, not a full decode.
 */
esp_err_t image_processor_peek_file_dimensions(const char *path, image_format_t format, int *out_w,
                                               int *out_h);

esp_err_t image_processor_peek_dimensions(const uint8_t *data, size_t size, image_format_t format,
                                          int *out_width, int *out_height);

/**
 * @brief Compose two source images into one canvas at the board's native
 * display resolution and process it exactly like a normal single image
 * (cover-fit each half, then CDR + dither).
 *
 * Used to combine two mismatched-orientation Telegram photos (e.g. two
 * portrait shots on a landscape frame) into one image instead of ever
 * displaying one alone.
 *
 * format_a/format_b each accept IMAGE_FORMAT_JPG, IMAGE_FORMAT_PNG, or
 * IMAGE_FORMAT_EPD_GZ (any other format returns ESP_ERR_NOT_SUPPORTED) - an
 * EPDGZ source decodes back to whichever palette color each pixel was
 * already dithered to, since that's all the format itself preserves.
 *
 * @param stack_vertically false = side-by-side (for a landscape-mounted
 * frame receiving portrait photos), true = stacked top/bottom (for a
 * portrait-mounted frame receiving landscape photos).
 */
esp_err_t image_processor_compose_pair_to_rgb(const uint8_t *data_a, size_t size_a,
                                              image_format_t format_a, const uint8_t *data_b,
                                              size_t size_b, image_format_t format_b,
                                              bool stack_vertically,
                                              dither_algorithm_t dither_algorithm,
                                              image_process_rgb_result_t *result);

// Per-line char buffer size shared by every text-overlay function in this
// module (captions, overlay bar lines, and image_processor_wrap_text()'s
// output) - a single public constant so callers can size their own arrays
// to match.
#define OVERLAY_LINE_MAX_CHARS 96

/**
 * @brief Overlays a caption bar (solid background + wrapped bitmap-font text)
 * across the bottom of an already-processed (dithered, palette-quantized)
 * RGB888 buffer. Uses the exact display palette so the result stays a valid
 * "processed" image. No-op if caption is NULL/empty.
 *
 * @param invert_colors false (default) = black bar, white text; true = white
 * bar, black text.
 */
void image_processor_draw_caption(uint8_t *rgb_buffer, int width, int height, const char *caption,
                                  bool invert_colors);

/**
 * @brief Same as image_processor_draw_caption(), but reads an already
 * display-processed PNG file, overlays the caption, and re-writes it in
 * place (no re-dithering). No-op (returns ESP_OK) if caption is NULL/empty.
 */
esp_err_t image_processor_add_caption_to_file(const char *png_path, const char *caption,
                                              bool invert_colors);

/**
 * @brief Draws a solid overlay bar across the TOP of an already-processed
 * RGB888 buffer, one independent line per entry in `lines` (each truncated
 * with an ellipsis to fit the display width - no word-wrap across lines,
 * unlike image_processor_draw_caption()/image_processor_wrap_text(); callers
 * that want a single long string wrapped across several of these lines
 * should pre-wrap it with image_processor_wrap_text() first). Top-anchored
 * specifically so it never collides with a caption bar (always
 * bottom-anchored) baked into the same image. Used for the weather/headline
 * overlay. No-op if lines is NULL or line_count <= 0.
 *
 * @param invert_colors false (default) = black bar, white text; true = white
 * bar, black text - Web UI toggle, independent of the caption bar's colors
 * (though callers may choose to pass the same value through).
 */
void image_processor_draw_overlay_bar(uint8_t *rgb_buffer, int width, int height,
                                      const char *const *lines, int line_count,
                                      bool invert_colors);

/**
 * @brief Same as image_processor_draw_overlay_bar(), but reads an already
 * display-processed PNG or EPDGZ file (auto-detected by content), draws the
 * overlay bar, and re-writes it in place (no re-dithering, same format as the
 * source - unless EPDGZ's deflate state can't be allocated, in which case
 * `path`'s extension is rewritten to ".png" to match where it actually got
 * written, mirroring image_processor_write_rgb_to_fmt()'s own contract).
 * `path` must be a mutable buffer (not a string literal) for that reason.
 * No-op (returns ESP_OK) if lines is NULL/empty.
 */
esp_err_t image_processor_add_overlay_to_file(char *path, const char *const *lines, int line_count,
                                              bool invert_colors);

/**
 * @brief Greedy word-wraps `text` into up to `max_lines` lines (each written
 * into `out_lines`, OVERLAY_LINE_MAX_CHARS bytes per row) that fit within
 * `width` pixels using the same font/wrap rules as
 * image_processor_draw_caption(). Truncates the last line with "..." if the
 * text doesn't fit within max_lines. Does NOT draw anything - just computes
 * the wrapped lines, so callers can assemble a mixed line list (e.g. one
 * long headline wrapped across several overlay lines, alongside other
 * already-single-line content) before handing everything to
 * image_processor_add_overlay_to_file(). ASCII-sanitizes internally, same as
 * the other text-overlay functions.
 *
 * @return Number of lines produced (0 if nothing renderable, e.g. `width`
 * too narrow for even one character).
 */
int image_processor_wrap_text(const char *text, int width, int max_lines,
                              char out_lines[][OVERLAY_LINE_MAX_CHARS]);

/**
 * @brief Transliterates UTF-8 (German umlauts/sz-ligature to ASCII digraphs,
 * everything else non-ASCII silently dropped) to plain ASCII - the same
 * sanitization image_processor_draw_caption()/draw_overlay_bar() apply
 * internally, exposed for callers that need ASCII-safe text before it
 * reaches this module (e.g. the headline RSS extractor, so oversized/raw
 * titles are already clean before line-length truncation decisions).
 */
void image_processor_sanitize_ascii(const char *utf8, char *out, size_t out_len);

/**
 * @brief Writes an already-processed RGB888 buffer to a PNG file. Thin
 * wrapper so callers outside this module (e.g. the Telegram orientation-pair
 * compositor) can persist a composed/captioned buffer without duplicating
 * libpng plumbing.
 */
/**
 * @brief Writes an already-processed RGB888 buffer to a PNG or EPDGZ file,
 * with the same quiet OOM-fallback-to-PNG contract as
 * image_processor_process_fmt()/image_processor_render_variant(): if EPDGZ's
 * deflate state can't be allocated, the file is written as PNG instead,
 * under output_path with its extension replaced by ".png" - out_actual_format
 * reports which actually happened so the caller can correct any path it
 * already committed to (e.g. a filename built before this call).
 */
esp_err_t image_processor_write_rgb_to_fmt(const uint8_t *rgb_buffer, int width, int height,
                                           const char *output_path, image_format_t out_format,
                                           image_format_t *out_actual_format);

esp_err_t image_processor_write_rgb_to_png(const uint8_t *rgb_buffer, int width, int height,
                                           const char *output_path);

/**
 * @brief Generates a small preview thumbnail from an already-processed PNG
 * or EPDGZ file (auto-detected by content), nearest-neighbor downsampled to
 * fit within max_dimension on its longer edge (aspect ratio preserved). Use
 * for images with no un-processed original available (e.g. a composed
 * orientation-pair) - anything with an original should use
 * image_processor_make_thumbnail_from_original() instead, so the preview
 * reflects true colors, not the e-paper palette.
 */
esp_err_t image_processor_make_thumbnail(const char *source_path, int max_dimension,
                                         const char *output_path);

/**
 * @brief Same as image_processor_make_thumbnail(), but decodes the source
 * directly (JPG or PNG) with no e-paper processing (no CDR, no dithering,
 * no palette quantization) - the thumbnail reflects the original photo's
 * true colors, not the low-color-depth e-paper output. Use this for any
 * source that still has an unprocessed original on disk (e.g. a freshly
 * downloaded Telegram photo, before it's converted to a display-ready PNG).
 */
esp_err_t image_processor_make_thumbnail_from_original(const char *source_path,
                                                        image_format_t format, int max_dimension,
                                                        const char *output_path);

#endif
