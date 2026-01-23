/**
 * CLI utility functions for image processing
 * Handles file loading, HEIC conversion, and EXIF orientation
 */

import fs from "fs";
import { loadImage, createCanvas } from "canvas";
import exifParser from "exif-parser";
import heicConvert from "heic-convert";
import { processImage, applyExifOrientation } from "./image-processor.js";

const DISPLAY_WIDTH = 800;
const DISPLAY_HEIGHT = 480;

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
 * Get EXIF orientation from image file
 * @param {string} imagePath - Path to image file
 * @returns {number} EXIF orientation value (1-8)
 */
function getExifOrientation(imagePath) {
  try {
    const buffer = fs.readFileSync(imagePath);
    const parser = exifParser.create(buffer);
    const result = parser.parse();
    return result.tags.Orientation || 1;
  } catch (error) {
    return 1;
  }
}

/**
 * Complete image processing pipeline from file to device-ready output
 * Loads image file, applies EXIF orientation, then processes through image-processor.js
 *
 * @param {string} imagePath - Path to image file
 * @param {Object} processingParams - Processing parameters
 * @param {Object} devicePalette - Optional device-specific palette
 * @param {Object} options - Additional options (verbose, skipDithering)
 * @returns {Promise<Object>} { canvas: processed canvas, originalCanvas: EXIF-corrected source }
 */
export async function processImagePipeline(
  imagePath,
  processingParams,
  devicePalette,
  options = {},
) {
  const { verbose = false, skipDithering = false } = options;

  // Load image (with HEIC conversion if needed)
  const img = await loadImageWithHeicSupport(imagePath);
  let canvas = createCanvas(img.width, img.height);
  const ctx = canvas.getContext("2d");
  ctx.drawImage(img, 0, 0);

  if (verbose) {
    console.log(`  Original size: ${canvas.width}x${canvas.height}`);
  }

  // Apply EXIF orientation
  const orientation = getExifOrientation(imagePath);
  if (orientation > 1) {
    if (verbose) {
      console.log(`  Applying EXIF orientation: ${orientation}`);
    }
    canvas = applyExifOrientation(canvas, orientation, createCanvas);
    if (verbose) {
      console.log(`  After EXIF correction: ${canvas.width}x${canvas.height}`);
    }
  }

  // Call shared processImage pipeline (handles rotation, resize, preprocessing, dithering)
  return processImage(canvas, processingParams, devicePalette, {
    verbose,
    targetWidth: DISPLAY_WIDTH,
    targetHeight: DISPLAY_HEIGHT,
    createCanvas,
    skipDithering,
  });
}
