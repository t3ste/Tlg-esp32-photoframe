/**
 * CLI utility functions for image processing
 * Handles file loading, HEIC conversion, and EXIF orientation
 */

import fs from "fs";
// @napi-rs/canvas (Skia-backed), not "canvas" (Cairo/libjpeg-turbo-backed) -
// confirmed via a real batch run + an isolated micro-benchmark that the
// latter leaks native memory on Windows that's invisible to Node's own
// memoryUsage() (heapUsed/external/arrayBuffers all stayed flat while RSS
// climbed ~50-100MB per processed photo, unrecoverable even with an explicit
// global.gc() after nulling every reference), eventually crashing a large
// batch (--crop-output both, --detect-faces) with "out of memory" partway
// through. @napi-rs/canvas has the same createCanvas/loadImage/Canvas2D
// surface this file and @aitjcize/epaper-image-convert's injected-createCanvas
// pipeline already expect, and doesn't exhibit this leak.
//
// It also, unlike node-canvas, already applies EXIF orientation itself while
// decoding a JPEG - confirmed directly against 400+ real photos (comparing
// the raw SOF-marker pixel dimensions against loadImage()'s own reported
// dimensions, plus two visual before/after checks) - so loadOrientedCanvas()
// below must NOT also apply it, or it doubles up: for a 90/270 rotation
// (EXIF Orientation 6/8) the image comes out sideways, for 180 (Orientation
// 3) it comes out upside down. This was a real, silent regression from the
// canvas-library swap above - the manual EXIF-correction step this file used
// to run after loadImage() was correct and necessary for node-canvas, which
// never touched EXIF orientation itself, but became actively harmful once
// @napi-rs/canvas started doing that same correction internally.
import { loadImage, createCanvas } from "@napi-rs/canvas";
import heicConvert from "heic-convert";
import {
  processImage,
  rotateImage,
  SPECTRA6,
  GRAYSCALE16,
} from "@aitjcize/epaper-image-convert";

/**
 * Load image with HEIC support
 * @param {string} imagePath - Path to image file
 * @returns {Promise<Image>} Loaded image
 */
async function loadImageWithHeicSupport(imagePath) {
  const ext = imagePath.toLowerCase();
  if (ext.endsWith(".heic") || ext.endsWith(".heif")) {
    const inputBuffer = fs.readFileSync(imagePath);
    const outputBuffer = await heicConvert({
      buffer: inputBuffer,
      format: "JPEG",
      quality: 1,
    });
    return await loadImage(outputBuffer);
  }
  return await loadImage(imagePath);
}

/**
 * Loads an image (with HEIC support) and optionally auto-rotates it 90° to
 * match the target orientation. This is the "upright, ready to crop/resize"
 * canvas - the same coordinate space face detection runs against for
 * face-aware cropping (see face-crop/).
 *
 * EXIF orientation is NOT applied manually here - @napi-rs/canvas's
 * loadImage() already decodes the image into upright, correctly-oriented
 * pixels on its own (see the import comment above for how this was
 * confirmed). loadImage()'s own img.width/height already reflect that.
 *
 * @param {string} imagePath - Path to image file
 * @param {Object} [options]
 * @param {boolean} [options.autoOrient=false] - Rotate 90° to match target orientation
 * @param {number} [options.displayWidth] - Target width, used by autoOrient
 * @param {number} [options.displayHeight] - Target height, used by autoOrient
 * @param {boolean} [options.verbose=false]
 * @returns {Promise<Canvas>}
 */
export async function loadOrientedCanvas(imagePath, options = {}) {
  const {
    autoOrient = false,
    displayWidth,
    displayHeight,
    verbose = false,
  } = options;

  const img = await loadImageWithHeicSupport(imagePath);
  let canvas = createCanvas(img.width, img.height);
  const ctx = canvas.getContext("2d");
  ctx.drawImage(img, 0, 0);

  if (autoOrient) {
    const isSourcePortrait = canvas.height > canvas.width;
    const isTargetPortrait = displayHeight > displayWidth;
    if (isSourcePortrait !== isTargetPortrait) {
      canvas = rotateImage(canvas, 90, createCanvas);
      if (verbose) {
        console.log(
          `  Auto-oriented: rotated 90° to match ${isTargetPortrait ? "portrait" : "landscape"} target`,
        );
      }
    }
  }

  return canvas;
}

/**
 * Process image pipeline: load, apply EXIF, process, return canvas
 * @param {string} imagePath - Path to image file
 * @param {Object} processingParams - Processing parameters
 * @param {number} displayWidth - Display width in pixels
 * @param {number} displayHeight - Display height in pixels
 * @param {Object} devicePalette - Optional device-specific palette
 * @param {Object} options - Processing options:
 *   - verbose {boolean} - Enable verbose logging
 *   - skipDithering {boolean} - Skip dithering step
 *   - autoOrient {boolean} - Auto-rotate image to match target orientation
 *   - orientation {string} - Display orientation: "landscape" or "portrait"
 *   - scaleMode {string} - Scale mode: "cover" or "fit" (default: "cover")
 *   - backgroundColor {string} - Palette color name for fit mode background (default: "white")
 *   - cropRect {{x:number,y:number,w:number,h:number}} - Optional pre-crop
 *     (in this function's own upright canvas pixel space, i.e. after EXIF
 *     correction and autoOrient) applied before resizing - used by the
 *     face-aware crop feature (face-crop/) to steer "cover" mode's crop
 *     instead of its default center-crop. Ignored if not given.
 * @returns {Promise<Object>} { canvas, originalCanvas, thumbnail }
 */
export async function processImagePipeline(
  imagePath,
  processingParams,
  displayWidth,
  displayHeight,
  devicePalette = null,
  options = {},
) {
  const {
    verbose = false,
    skipDithering = false,
    autoOrient = false,
    orientation = "landscape",
    scaleMode = "cover",
    backgroundColor = "white",
    usePerceivedOutput = false,
    grayscale = false,
    cropRect = null,
  } = options;

  let canvas = await loadOrientedCanvas(imagePath, {
    autoOrient,
    displayWidth,
    displayHeight,
    verbose,
  });

  if (cropRect) {
    const { x, y, w, h } = cropRect;
    const cropped = createCanvas(w, h);
    cropped.getContext("2d").drawImage(canvas, x, y, w, h, 0, 0, w, h);
    if (verbose) {
      console.log(`  Applying face-aware crop: ${w}x${h} at (${x},${y})`);
    }
    canvas = cropped;
  }

  // Build palette object for the library
  // The library expects { theoretical, perceived } format
  // devicePalette from the device is the "perceived" palette
  let palette;
  if (grayscale) {
    // 16-level grayscale (GC16 / IT8951): dither against the gray ramp.
    palette = GRAYSCALE16;
  } else if (devicePalette) {
    // Use SPECTRA6 theoretical with device-provided perceived palette
    palette = {
      theoretical: SPECTRA6.theoretical,
      perceived: devicePalette,
    };
  } else {
    // Use default SPECTRA6 palette
    palette = SPECTRA6;
  }

  // Call shared processImage pipeline (handles rotation, resize, preprocessing, dithering).
  //
  // Passed as ImageData, not the raw canvas, even though processImage() accepts
  // either: it picks between them via `source.data && source.width && source.height`
  // (processor.js), which assumes a Canvas never has a truthy `.data` - true for
  // node-canvas, but @napi-rs/canvas's Canvas exposes its own `.data` accessor
  // (a `[Function: data]`, always truthy), so a raw canvas here gets misdetected
  // as ImageData and handed to putImageData() instead of drawImage() - silently
  // producing a blank output (all-black for "cover", all-white/background-color
  // for "fit", since nothing ever actually got drawn: confirmed on real output,
  // both epdgz files shrank to ~220 bytes - gzip of one solid color - and both
  // thumbnails came back as flat black/white). Passing genuine ImageData sidesteps
  // the ambiguity entirely - it satisfies that check unambiguously on any canvas
  // implementation, which is exactly the codepath the check was trying to select.
  const sourceImageData = canvas
    .getContext("2d")
    .getImageData(0, 0, canvas.width, canvas.height);

  return processImage(sourceImageData, {
    displayWidth,
    displayHeight,
    palette,
    params: processingParams,
    orientation,
    scaleMode,
    backgroundColor,
    verbose,
    createCanvas,
    skipDithering,
    usePerceivedOutput,
  });
}
