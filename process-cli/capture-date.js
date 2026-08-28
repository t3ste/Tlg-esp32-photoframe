/**
 * Versioned per-image capture-date sidecar (`<basename>.capture.json`).
 *
 * Deliberately a separate file from `face-crop/metadata.js`'s `.facecrop.json`
 * (not folded into it) - that sidecar is only written when `--detect-faces` is
 * used, and a capture date shouldn't require face detection to be enabled.
 * Same one-file-per-source-image rationale as its sibling: robust against
 * files being moved/copied independently, trivial for the firmware to
 * associate with its source image, no database to keep in sync.
 *
 * The original camera JPEG's EXIF is the only place a capture date can come
 * from - once a photo is dithered/palette-quantized into a display-ready
 * PNG/EPDGZ/BMP, that information is gone for good, and (per this project's
 * own documented SD-card convention) the original is generally never kept on
 * the device at all. So this has to be captured here, at processing time, not
 * reconstructed later on-device.
 */

import fs from "fs";
import path from "path";
import ExifReader from "exifreader";

export const CAPTURE_DATE_SCHEMA_VERSION = 1;
export const CAPTURE_DATE_SUFFIX = ".capture.json";

/** Derives the capture-date sidecar path for a given output image path. */
export function captureDatePathFor(outputImagePath) {
  const dir = path.dirname(outputImagePath);
  const base = path.basename(outputImagePath, path.extname(outputImagePath));
  return path.join(dir, `${base}${CAPTURE_DATE_SUFFIX}`);
}

/**
 * Reads `sourcePath`'s EXIF DateTimeOriginal tag, if present, and reformats
 * it from EXIF's own "YYYY:MM:DD HH:MM:SS" to "YYYY-MM-DD HH:MM" (seconds
 * dropped) - matching main/exif_reader.c's exact existing convention for the
 * same tag, so the firmware can use this string as a caption verbatim, with
 * no date parsing of its own needed.
 *
 * @returns {string|null} The reformatted date, or null if absent/unparseable/
 *   unreadable - never throws (a source with no EXIF is the common case, not
 *   an error).
 */
export function extractCaptureDate(sourcePath) {
  try {
    const buffer = fs.readFileSync(sourcePath);
    const tags = ExifReader.load(buffer);
    const raw = tags?.DateTimeOriginal?.value;
    // exifreader may return either the raw string or a single-element array
    // depending on the tag's encoded count - normalize both.
    const value = Array.isArray(raw) ? raw[0] : raw;
    if (typeof value !== "string") {
      return null;
    }
    const m = value.match(/^(\d{4}):(\d{2}):(\d{2}) (\d{2}):(\d{2}):(\d{2})$/);
    if (!m) {
      return null;
    }
    const [, year, month, day, hour, minute] = m;
    return `${year}-${month}-${day} ${hour}:${minute}`;
  } catch {
    return null;
  }
}

/** Writes the capture-date sidecar (overwrites any existing file). */
export function writeCaptureDateFile(captureDatePath, captureDate) {
  const metadata = { schema: CAPTURE_DATE_SCHEMA_VERSION, capture_date: captureDate };
  fs.writeFileSync(captureDatePath, JSON.stringify(metadata, null, 2) + "\n");
}
