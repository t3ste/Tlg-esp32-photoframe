#!/usr/bin/env node

import { Command } from "commander";
import fs from "fs";
import path from "path";
import os from "os";
import http from "http";
import { fileURLToPath } from "url";
import { createCanvas } from "@napi-rs/canvas";
import FormData from "form-data";
import {
  generateThumbnail,
  createPNG,
  createEPDGZ,
  getPreset,
  getPresetNames,
  getDefaultParams,
} from "@aitjcize/epaper-image-convert";
import { processImagePipeline, loadOrientedCanvas } from "./utils.js";
import { createImageServer } from "./server.js";
import {
  normalizeTargetGeometry,
  getBoardProfile,
  getFaceDetector,
  analyzeFaceCrop,
  buildMetadata,
  writeMetadataFile,
  metadataPathFor,
} from "./face-crop/index.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const THUMBNAIL_MAX_DIM = 400;
const CLI_VERSION = "1.0.0";
const GENERATOR_VERSION = `esp32-photoframe-cli@${CLI_VERSION}`;

// Get default parameters from the library
const DEFAULT_PARAMS = {
  ...getDefaultParams(),
};

// Fetch processing settings from device
async function fetchDeviceSettings(host) {
  return new Promise((resolve, reject) => {
    const url = `http://${host}/api/settings/processing`;
    console.log(`Fetching settings from device: ${url}`);

    http
      .get(url, (res) => {
        let data = "";

        res.on("data", (chunk) => {
          data += chunk;
        });

        res.on("end", () => {
          if (res.statusCode === 200) {
            try {
              const settings = JSON.parse(data);
              console.log("Device settings loaded successfully");
              console.log(
                `  exposure=${settings.exposure}, saturation=${settings.saturation}, tone_mode=${settings.toneMode}`,
              );
              if (settings.toneMode === "scurve") {
                console.log(
                  `  scurve: strength=${settings.strength}, shadow=${settings.shadowBoost}, highlight=${settings.highlightCompress}, midpoint=${settings.midpoint}`,
                );
              } else if (settings.toneMode === "contrast") {
                console.log(`  contrast=${settings.contrast}`);
              }
              console.log(
                `  dither_algorithm=${settings.ditherAlgorithm || "floyd-steinberg"}`,
              );
              resolve(settings);
            } catch (error) {
              reject(
                new Error(`Failed to parse device settings: ${error.message}`),
              );
            }
          } else {
            reject(
              new Error(
                `HTTP ${res.statusCode}: Failed to fetch settings from device`,
              ),
            );
          }
        });
      })
      .on("error", (error) => {
        reject(
          new Error(`Failed to connect to device at ${host}: ${error.message}`),
        );
      });
  });
}

// Fetch color palette from device
async function fetchDevicePalette(host) {
  return new Promise((resolve, reject) => {
    const url = `http://${host}/api/settings/palette`;
    console.log(`Fetching color palette from device: ${url}`);

    http
      .get(url, (res) => {
        let data = "";

        res.on("data", (chunk) => {
          data += chunk;
        });

        res.on("end", () => {
          if (res.statusCode === 200) {
            try {
              const palette = JSON.parse(data);
              console.log("Device color palette loaded successfully");
              resolve(palette);
            } catch (error) {
              reject(
                new Error(`Failed to parse device palette: ${error.message}`),
              );
            }
          } else {
            reject(
              new Error(
                `HTTP ${res.statusCode}: Failed to fetch palette from device`,
              ),
            );
          }
        });
      })
      .on("error", (error) => {
        reject(
          new Error(`Failed to connect to device at ${host}: ${error.message}`),
        );
      });
  });
}

// Minimum firmware version that supports epdgz format
const MIN_EPDGZ_VERSION = "2.6.1";

// Compare two semver strings (with optional "v" prefix).
// Returns -1 if v1 < v2, 0 if equal, 1 if v1 > v2.
function compareVersions(v1, v2) {
  v1 = v1.replace(/^v/, "");
  v2 = v2.replace(/^v/, "");
  if (v1.startsWith("dev-")) return -1;
  if (v2.startsWith("dev-")) return 1;
  const p1 = v1.split(".").map(Number);
  const p2 = v2.split(".").map(Number);
  for (let i = 0; i < 3; i++) {
    if ((p1[i] || 0) < (p2[i] || 0)) return -1;
    if ((p1[i] || 0) > (p2[i] || 0)) return 1;
  }
  return 0;
}

function supportsEPDGZ(version) {
  return version && compareVersions(version, MIN_EPDGZ_VERSION) > 0;
}

// Fetch device system info (resolution + version)

async function fetchDeviceSystemInfo(host) {
  return new Promise((resolve, reject) => {
    const url = `http://${host}/api/system-info`;
    console.log(`Fetching system info from device: ${url}`);

    http
      .get(url, (res) => {
        let data = "";

        res.on("data", (chunk) => {
          data += chunk;
        });

        res.on("end", () => {
          if (res.statusCode === 200) {
            try {
              const info = JSON.parse(data);
              if (info.width && info.height) {
                console.log(
                  `Device display resolution: ${info.width}x${info.height}`,
                );
                if (info.version) {
                  console.log(`Device firmware version: ${info.version}`);
                }
                resolve({
                  width: info.width,
                  height: info.height,
                  version: info.version || "",
                });
              } else {
                reject(
                  new Error("Device system info does not contain width/height"),
                );
              }
            } catch (error) {
              reject(
                new Error(
                  `Failed to parse device system info: ${error.message}`,
                ),
              );
            }
          } else {
            reject(
              new Error(
                `HTTP ${res.statusCode}: Failed to fetch system info from device`,
              ),
            );
          }
        });
      })
      .on("error", (error) => {
        reject(
          new Error(`Failed to connect to device at ${host}: ${error.message}`),
        );
      });
  });
}

// Fetch display orientation from device settings
async function fetchDeviceOrientation(host) {
  return new Promise((resolve, reject) => {
    const url = `http://${host}/api/config`;
    console.log(`Fetching display orientation from device: ${url}`);

    http
      .get(url, (res) => {
        let data = "";

        res.on("data", (chunk) => {
          data += chunk;
        });

        res.on("end", () => {
          if (res.statusCode === 200) {
            try {
              const settings = JSON.parse(data);
              const orientation = settings.display_orientation || "landscape";
              console.log(`Device display orientation: ${orientation}`);
              resolve(orientation);
            } catch (error) {
              resolve("landscape");
            }
          } else {
            resolve("landscape");
          }
        });
      })
      .on("error", () => {
        resolve("landscape");
      });
  });
}

// Display image directly on device without saving (via /api/display-image)
async function displayDirectly(host, pngPath, thumbPath, retries = 3) {
  for (let attempt = 1; attempt <= retries; attempt++) {
    try {
      if (attempt > 1) {
        console.log(
          `  Retry attempt ${attempt}/${retries} after 5 second delay...`,
        );
        await new Promise((resolve) => setTimeout(resolve, 5000));
      }

      await displayDirectlyOnce(host, pngPath, thumbPath);
      return; // Success, exit retry loop
    } catch (error) {
      if (attempt === retries) {
        throw error; // Last attempt failed, throw error
      }
      console.log(`  Display failed: ${error.message}`);
    }
  }
}

// Single direct display attempt
function displayDirectlyOnce(host, imagePath, thumbPath) {
  return new Promise((resolve, reject) => {
    console.log(`Displaying image directly on device: ${host}`);
    console.log(`  Image: ${imagePath}`);
    console.log(`  Thumbnail: ${thumbPath}`);

    const contentType = imagePath.endsWith(".epdgz")
      ? "application/octet-stream"
      : "image/png";
    const form = new FormData();
    form.append("image", fs.createReadStream(imagePath), {
      filename: path.basename(imagePath),
      contentType,
    });
    form.append("thumbnail", fs.createReadStream(thumbPath), {
      filename: path.basename(thumbPath),
      contentType: "image/jpeg",
    });

    // Use form.submit() which properly handles Content-Length
    form
      .submit(
        {
          protocol: "http:",
          host: host,
          port: 80,
          path: "/api/display-image",
          method: "POST",
        },
        (err, res) => {
          if (err) {
            reject(new Error(`Failed to submit form: ${err.message}`));
            return;
          }

          let data = "";

          res.on("data", (chunk) => {
            data += chunk;
          });

          res.on("end", () => {
            if (res.statusCode === 200) {
              console.log(`✓ Image displayed successfully`);
              resolve();
            } else {
              reject(
                new Error(`Server returned status ${res.statusCode}: ${data}`),
              );
            }
          });

          res.on("error", (error) => {
            reject(new Error(`Failed to read response: ${error.message}`));
          });
        },
      )
      .on("error", (error) => {
        reject(
          new Error(`Failed to connect to device at ${host}: ${error.message}`),
        );
      });
  });
}

// Derive a short, unique upload basename from the local file's name +
// current timestamp so the device-side album avoids collisions when many
// sources share a filename (e.g. "IMG_1234.jpg"). 32-bit FNV-1a over the
// payload plus a base-36 millisecond suffix = 12 chars. Same algorithm
// as the mobile app and firmware webapp, so the scheme stays consistent
// across clients and has no runtime crypto-API dependency.
function hashedUploadBasename(localPath) {
  const source = path.basename(localPath, path.extname(localPath));
  const ts = Date.now();
  const payload = `${source}:${ts}`;
  let fnv = 0x811c9dc5;
  for (let i = 0; i < payload.length; i++) {
    fnv ^= payload.charCodeAt(i);
    fnv = Math.imul(fnv, 0x01000193);
  }
  return (fnv >>> 0).toString(16).padStart(8, "0") + ts.toString(36).slice(-4);
}

// Upload PNG and thumbnail to device with retry logic
async function uploadToDevice(
  host,
  pngPath,
  thumbPath,
  album = null,
  retries = 3,
) {
  // Compute the device-side basename once so the image and its thumbnail
  // end up paired under the same hash across retries.
  const uploadBasename = hashedUploadBasename(pngPath);

  for (let attempt = 1; attempt <= retries; attempt++) {
    try {
      if (attempt > 1) {
        console.log(
          `  Retry attempt ${attempt}/${retries} after 5 second delay...`,
        );
        await new Promise((resolve) => setTimeout(resolve, 5000));
      }

      await uploadToDeviceOnce(host, pngPath, thumbPath, album, uploadBasename);
      return; // Success, exit retry loop
    } catch (error) {
      if (attempt === retries) {
        throw error; // Last attempt failed, throw error
      }
      console.log(`  Upload failed: ${error.message}`);
    }
  }
}

// Single upload attempt
function uploadToDeviceOnce(
  host,
  imagePath,
  thumbPath,
  album = null,
  uploadBasename = null,
) {
  return new Promise((resolve, reject) => {
    console.log(`Uploading to device: ${host}`);
    console.log(`  Image: ${imagePath}`);
    console.log(`  Thumbnail: ${thumbPath}`);
    if (album) {
      console.log(`  Album: ${album}`);
    }

    const imageExt = path.extname(imagePath);
    const contentType = imagePath.endsWith(".epdgz")
      ? "application/octet-stream"
      : "image/png";
    const base = uploadBasename || path.basename(imagePath, imageExt);
    const form = new FormData();
    form.append("image", fs.createReadStream(imagePath), {
      filename: `${base}${imageExt}`,
      contentType,
    });
    form.append("thumbnail", fs.createReadStream(thumbPath), {
      filename: `${base}${path.extname(thumbPath)}`,
      contentType: "image/jpeg",
    });

    // Build path with album query parameter if provided
    let uploadPath = "/api/upload";
    if (album) {
      uploadPath += `?album=${encodeURIComponent(album)}`;
    }

    // Use form.submit() which properly handles Content-Length
    form.submit(
      {
        protocol: "http:",
        host: host,
        port: 80,
        path: uploadPath,
        method: "POST",
      },
      (err, res) => {
        if (err) {
          reject(new Error(`Failed to submit form: ${err.message}`));
          return;
        }

        let data = "";

        res.on("data", (chunk) => {
          data += chunk;
        });

        res.on("end", () => {
          if (res.statusCode === 200) {
            try {
              const response = JSON.parse(data);
              console.log(`✓ Upload successful: ${response.filepath}`);
              resolve(response);
            } catch (error) {
              reject(new Error(`Failed to parse response: ${error.message}`));
            }
          } else {
            reject(
              new Error(
                `HTTP ${res.statusCode}: Upload failed - ${data || "Unknown error"}`,
              ),
            );
          }
        });

        res.on("error", (error) => {
          reject(new Error(`Response error: ${error.message}`));
        });
      },
    );
  });
}

// BMP file writing (24-bit RGB format)
function writeBMP(imageData, outputPath) {
  const width = imageData.width;
  const height = imageData.height;
  const data = imageData.data;

  // BMP header for 24-bit RGB
  const fileHeaderSize = 14;
  const infoHeaderSize = 40;
  const headerSize = fileHeaderSize + infoHeaderSize;
  const rowSize = Math.floor((width * 3 + 3) / 4) * 4; // 3 bytes per pixel, padded to multiple of 4
  const imageSize = rowSize * height;
  const fileSize = headerSize + imageSize;

  const buffer = Buffer.alloc(fileSize);
  let offset = 0;

  // File header (14 bytes)
  buffer.write("BM", offset);
  offset += 2;
  buffer.writeUInt32LE(fileSize, offset);
  offset += 4;
  buffer.writeUInt32LE(0, offset);
  offset += 4; // Reserved
  buffer.writeUInt32LE(headerSize, offset);
  offset += 4;

  // Info header (40 bytes)
  buffer.writeUInt32LE(infoHeaderSize, offset);
  offset += 4;
  buffer.writeInt32LE(width, offset);
  offset += 4;
  buffer.writeInt32LE(height, offset);
  offset += 4;
  buffer.writeUInt16LE(1, offset);
  offset += 2; // Planes
  buffer.writeUInt16LE(24, offset);
  offset += 2; // Bits per pixel (24-bit RGB)
  buffer.writeUInt32LE(0, offset);
  offset += 4; // Compression (none)
  buffer.writeUInt32LE(imageSize, offset);
  offset += 4;
  buffer.writeInt32LE(2835, offset);
  offset += 4; // X pixels per meter
  buffer.writeInt32LE(2835, offset);
  offset += 4; // Y pixels per meter
  buffer.writeUInt32LE(0, offset);
  offset += 4; // Colors used (0 = all colors)
  buffer.writeUInt32LE(0, offset);
  offset += 4; // Important colors

  // Pixel data (bottom-up, left-to-right, BGR format)
  for (let y = height - 1; y >= 0; y--) {
    for (let x = 0; x < width; x++) {
      const idx = (y * width + x) * 4;
      const r = data[idx];
      const g = data[idx + 1];
      const b = data[idx + 2];

      // Write BGR (BMP format)
      buffer.writeUInt8(b, offset++);
      buffer.writeUInt8(g, offset++);
      buffer.writeUInt8(r, offset++);
    }

    // Padding to make row size multiple of 4
    const padding = rowSize - width * 3;
    for (let i = 0; i < padding; i++) {
      buffer.writeUInt8(0, offset++);
    }
  }

  fs.writeFileSync(outputPath, buffer);
}

// Check if file is an image
function isImageFile(filename) {
  const ext = path.extname(filename).toLowerCase();
  return [
    ".jpg",
    ".jpeg",
    ".png",
    ".gif",
    ".bmp",
    ".webp",
    ".heic",
    ".heif",
  ].includes(ext);
}

// Process all images in a folder structure (albums)
async function processFolderStructure(
  inputDir,
  outputDir,
  options,
  devicePalette,
  uploadHost = null,
) {
  console.log(`\nProcessing folder structure: ${inputDir}`);
  console.log(`Output directory: ${outputDir}\n`);

  // Read all subdirectories (albums)
  const entries = fs.readdirSync(inputDir, { withFileTypes: true });
  const albums = entries.filter((entry) => entry.isDirectory());

  if (albums.length === 0) {
    console.log("No subdirectories (albums) found in input directory.");
    return;
  }

  console.log(
    `Found ${albums.length} album(s): ${albums.map((a) => a.name).join(", ")}\n`,
  );

  let totalProcessed = 0;
  let totalUploaded = 0;
  let totalErrors = 0;

  for (const album of albums) {
    const albumName = album.name;
    const albumInputPath = path.join(inputDir, albumName);
    const albumOutputPath = path.join(outputDir, albumName);

    console.log(`\n=== Processing album: ${albumName} ===`);

    // Create output directory for this album (always create, even if tmpdir)
    fs.mkdirSync(albumOutputPath, { recursive: true });

    // Get all image files in this album
    const files = fs.readdirSync(albumInputPath);
    const imageFiles = files.filter(isImageFile);

    if (imageFiles.length === 0) {
      console.log(`  No images found in album: ${albumName}`);
      continue;
    }

    console.log(`  Found ${imageFiles.length} image(s)`);

    // Process each image
    for (let i = 0; i < imageFiles.length; i++) {
      const imageFile = imageFiles[i];
      const inputPath = path.join(albumInputPath, imageFile);
      const baseName = path.basename(imageFile, path.extname(imageFile));
      const fmt = options.format || "epdgz";
      const ext = fmt === "bmp" ? ".bmp" : fmt === "png" ? ".png" : ".epdgz";
      const outputBasePath = path.join(albumOutputPath, baseName);

      try {
        console.log(
          `  [${i + 1}/${imageFiles.length}] Processing: ${imageFile}`,
        );
        const rendered = await processImageFile(
          inputPath,
          outputBasePath,
          ext,
          options,
          devicePalette,
        );
        totalProcessed++;

        // Upload if requested (rendered is empty for --metadata-only, and
        // never has more than one entry here - --crop-output both is
        // rejected together with --upload earlier)
        if (uploadHost && rendered.length > 0) {
          const { outputFile, outputThumb } = rendered[0];
          try {
            await uploadToDevice(
              uploadHost,
              outputFile,
              outputThumb,
              albumName,
            );
            totalUploaded++;
          } catch (error) {
            console.error(`  ERROR uploading ${imageFile}: ${error.message}`);
            totalErrors++;
          }
        }
      } catch (error) {
        console.error(`  ERROR processing ${imageFile}: ${error.message}`);
        totalErrors++;
      }
    }
  }

  console.log(`\n=== Summary ===`);
  console.log(`Total images processed: ${totalProcessed}`);
  if (uploadHost) {
    console.log(`Total images uploaded: ${totalUploaded}`);
  }
  if (totalErrors > 0) {
    console.log(`Total errors: ${totalErrors}`);
  }
  console.log(`Output directory: ${outputDir}`);
}

// Renders one variant (a specific scaleMode/cropRect combination) to
// `outputFile` (+ its thumbnail at `outputThumb`), via the shared library
// pipeline.
async function renderVariant(
  inputPath,
  outputFile,
  outputThumb,
  processingOptions,
  devicePalette,
  { scaleMode, cropRect },
) {
  const { canvas, originalCanvas } = await processImagePipeline(
    inputPath,
    processingOptions,
    processingOptions.displayWidth,
    processingOptions.displayHeight,
    devicePalette,
    {
      verbose: processingOptions.verbose || true,
      autoOrient: processingOptions.autoOrient || false,
      orientation: processingOptions.orientation || "landscape",
      scaleMode,
      backgroundColor: processingOptions.backgroundColor || "white",
      usePerceivedOutput: processingOptions.usePerceivedOutput || false,
      grayscale: processingOptions.grayscale || false,
      cropRect,
    },
  );

  const ctx = canvas.getContext("2d");
  const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);

  const format = processingOptions.format || "epdgz";
  if (format === "epdgz") {
    console.log(`  Writing EPDGZ: ${outputFile}`);
    const epdBuffer = await createEPDGZ(canvas, {
      grayscale: processingOptions.grayscale || false,
    });
    fs.writeFileSync(outputFile, epdBuffer);
  } else if (format === "png") {
    console.log(`  Writing PNG: ${outputFile}`);
    const pngBuffer = await createPNG(canvas);
    fs.writeFileSync(outputFile, pngBuffer);
  } else if (format === "bmp") {
    console.log(`  Writing BMP: ${outputFile}`);
    writeBMP(imageData, outputFile);
  } else {
    throw new Error(`Unsupported format: ${format}. Use 'epdgz', 'png', or 'bmp'`);
  }

  if (processingOptions.generateThumbnail && outputThumb) {
    console.log(`  Generating thumbnail: ${outputThumb}`);
    // Generate thumbnail from this variant's own clean, unprocessed source -
    // e.g. the "fit" variant's thumbnail correctly shows the full letterboxed
    // photo, not the "cover" variant's crop.
    const thumbCanvas = generateThumbnail(originalCanvas, THUMBNAIL_MAX_DIM, createCanvas);
    const buffer = thumbCanvas.toBuffer("image/jpeg", { quality: 0.8 });
    fs.writeFileSync(outputThumb, buffer);
  }
}

/**
 * Processes one source image: optional face detection + metadata, then one
 * or more rendered variants depending on processingOptions.faceCrop.cropOutput
 * ("cropped" | "uncropped" | "both") - or a single plain render, unchanged
 * from before this feature existed, when face-crop isn't enabled at all.
 *
 * @param {string} inputPath
 * @param {string} outputBasePath - Output path with no extension (directory +
 *   basename + any user --suffix already applied) - variant suffixes
 *   (".cover"/".fit") and the metadata's ".facecrop.json" are appended here.
 * @param {string} ext - Output image extension, e.g. ".png".
 * @param {Object} processingOptions
 * @param {Object} [devicePalette]
 * @returns {Promise<Array<{outputFile: string, outputThumb: string}>>} The
 *   rendered variant(s) - empty when --metadata-only skipped rendering.
 */
async function processImageFile(inputPath, outputBasePath, ext, processingOptions, devicePalette = null) {
  console.log(`Processing: ${inputPath}`);

  let recommendedCrop = null;
  if (processingOptions.faceCrop?.enabled) {
    const { target, marginPercent, detector, engineName, metadataOnly } = processingOptions.faceCrop;

    const orientedCanvas = await loadOrientedCanvas(inputPath, {
      autoOrient: processingOptions.autoOrient || false,
      displayWidth: processingOptions.displayWidth,
      displayHeight: processingOptions.displayHeight,
      verbose: processingOptions.verbose || true,
    });
    const imageData = orientedCanvas
      .getContext("2d")
      .getImageData(0, 0, orientedCanvas.width, orientedCanvas.height);

    console.log(`  Detecting faces (${engineName})...`);
    const analyzed = await analyzeFaceCrop({ detector, imageData, target, marginPercent, engineName });
    console.log(`  Found ${analyzed.faces.length} face(s)`);

    const metadata = buildMetadata({
      sourcePath: path.basename(inputPath),
      image: { width: orientedCanvas.width, height: orientedCanvas.height },
      target,
      faces: analyzed.faces,
      recommendedCrop: analyzed.recommendedCrop,
      strategy: analyzed.strategy,
      generatorVersion: GENERATOR_VERSION,
    });
    // Always named after the plain base path (never a .cover/.fit variant
    // suffix) - one metadata file describes the source image regardless of
    // how many rendered variants exist for it.
    const metadataPath = metadataPathFor(`${outputBasePath}${ext}`);
    writeMetadataFile(metadataPath, metadata);
    console.log(`  Wrote face-crop metadata: ${metadataPath}`);

    if (metadataOnly) {
      console.log(`Done! (metadata only)`);
      return [];
    }

    recommendedCrop = analyzed.recommendedCrop;
  }

  // Which variant(s) to render. Without --detect-faces, this is exactly the
  // single render this CLI has always produced - scaleMode/cropRect
  // untouched, zero behavior change.
  let variants;
  if (!processingOptions.faceCrop?.enabled) {
    variants = [{ suffix: "", scaleMode: processingOptions.scaleMode || "cover", cropRect: null }];
  } else {
    const cropOutput = processingOptions.faceCrop.cropOutput;
    if (cropOutput === "uncropped") {
      variants = [{ suffix: "", scaleMode: "fit", cropRect: null }];
    } else if (cropOutput === "both") {
      variants = [
        { suffix: ".cover", scaleMode: "cover", cropRect: recommendedCrop },
        { suffix: ".fit", scaleMode: "fit", cropRect: null },
      ];
    } else {
      // "cropped" (default)
      variants = [{ suffix: "", scaleMode: "cover", cropRect: recommendedCrop }];
    }
  }

  const rendered = [];
  for (const variant of variants) {
    const outputFile = `${outputBasePath}${variant.suffix}${ext}`;
    const outputThumb = `${outputBasePath}${variant.suffix}.jpg`;
    await renderVariant(inputPath, outputFile, outputThumb, processingOptions, devicePalette, variant);
    rendered.push({ outputFile, outputThumb });
  }

  console.log(`Done!`);
  return rendered;
}

// CLI setup
const program = new Command();

program
  .name("photoframe-process")
  .description("ESP32 PhotoFrame image processing CLI")
  .version(CLI_VERSION)
  .argument(
    "<input>",
    "Input image file or directory with album subdirectories",
  )
  .option("-o, --output-dir <dir>", "Output directory", ".")
  .option(
    "--suffix <suffix>",
    "Suffix to add to output filename (single file mode only)",
    "",
  )
  .option("-v, --verbose", "Enable verbose logging")
  .option("--format <format>", "Output format: epdgz, png, or bmp", "epdgz")
  .option(
    "--grayscale",
    "Pack output as 16-level grayscale (GC16 / IT8951 panels)",
  )
  .option(
    "--preset <name>",
    `Processing preset: ${getPresetNames().join(", ")} `,
    "balanced",
  )
  .option(
    "--upload",
    "Upload converted PNG and thumbnail to device (requires --host)",
  )
  .option(
    "--direct",
    "Display image directly on device without saving (requires --host, single file only)",
  )
  .option(
    "--serve",
    "Start HTTP server to serve images from album directory structure",
  )
  .option("--serve-port <port>", "Port for HTTP server in --serve mode", "8080")
  .option(
    "--serve-format <format>",
    "Image format to serve: epdgz, png, jpg, or bmp",
    "epdgz",
  )
  .option(
    "--host <host>",
    "Device hostname or IP address (default: photoframe.local)",
    "photoframe.local",
  )
  .option("--device-parameters", "Fetch processing parameters from device")
  .option(
    "--exposure <value>",
    "Exposure multiplier (0.5-2.0, 1.0=normal)",
    parseFloat,
  )
  .option(
    "--saturation <value>",
    "Saturation multiplier (0.5-2.0, 1.0=normal)",
    parseFloat,
  )
  .option("--tone-mode <mode>", "Tone mapping mode: scurve or contrast")
  .option(
    "--contrast <value>",
    "Contrast multiplier for simple mode (0.5-2.0, 1.0=normal)",
    parseFloat,
  )
  .option(
    "--scurve-strength <value>",
    "S-curve overall strength (0.0-1.0)",
    parseFloat,
  )
  .option(
    "--scurve-shadow <value>",
    "S-curve shadow boost (0.0-1.0)",
    parseFloat,
  )
  .option(
    "--scurve-highlight <value>",
    "S-curve highlight compress (0.5-5.0)",
    parseFloat,
  )
  .option("--scurve-midpoint <value>", "S-curve midpoint (0.3-0.7)", parseFloat)
  .option("--color-method <method>", "Color matching: rgb or lab")
  .option(
    "--use-perceived-output",
    "Use perceived (measured) palette colors in output for realistic preview",
  )
  .option(
    "--dither-algorithm <algorithm>",
    "Dithering algorithm: floyd-steinberg, stucki, burkes, or sierra",
  )
  .option(
    "--auto-orient",
    "Auto-rotate images to match target display orientation",
  )
  .option(
    "--orientation <mode>",
    "Display orientation: landscape or portrait (overridden by --device-parameters)",
    "landscape",
  )
  .option(
    "--scale-mode <mode>",
    "Scale mode: cover (crop to fill) or fit (letterbox)",
    "cover",
  )
  .option(
    "--background-color <name>",
    "Background palette color for fit mode (black, white, etc.)",
    "white",
  )
  .option("--display-width <width>", "Display width in pixels", parseInt, 800)
  .option(
    "--display-height <height>",
    "Display height in pixels",
    parseInt,
    480,
  )
  .option(
    "-d, --dimension <WxH>",
    "Display dimension (e.g., 800x480) - overrides display-width/height",
  )
  .option("--compress-dynamic-range", "Compress dynamic range to display range")
  .option("--no-compress-dynamic-range", "Disable dynamic range compression")
  .option(
    "--detect-faces",
    "Detect faces and write a <name>.facecrop.json metadata file next to the output " +
      "(see docs/FACE_CROP.md); also steers the rendered image's crop toward keeping large faces visible",
  )
  .option(
    "--metadata-only",
    "With --detect-faces: write only the <name>.facecrop.json file, skip generating the rendered output image",
  )
  .option(
    "--crop-output <mode>",
    "With --detect-faces (ignored if --metadata-only is also given): which rendered image(s) to " +
      "produce - 'cropped' (default: one face-aware-cropped image, cover mode), 'uncropped' (one " +
      "full/letterboxed image, fit mode, no crop applied - metadata is still written), or 'both' " +
      "(<name>.cover.<ext> and <name>.fit.<ext> side by side, so the firmware can later pick the " +
      "right one for its Cover/Fit setting without rendering anything itself - see docs/FACE_CROP.md)",
    "cropped",
  )
  .option(
    "--board <id>",
    "Target board id for face-crop geometry (see boards/boards.json); also sets " +
      "the display resolution unless --resolution/--dimension/--display-width/--display-height override it",
  )
  .option(
    "--resolution <WxH>",
    "Target display resolution in pixels, e.g. 800x480 (alias of --dimension, also used as the face-crop target)",
  )
  .option(
    "--display-size-mm <WxH>",
    "Physical display size in mm, e.g. 160x96 - used only to help auto-derive orientation for face-crop",
  )
  .option(
    "--face-margin <percent>",
    "Safety margin added around each detected face, as a fraction of its own size",
    parseFloat,
    0.12,
  )
  .option(
    "--face-min-score <value>",
    "Minimum face detection confidence to keep a face (0.0-1.0)",
    parseFloat,
    0.75,
  )
  .option(
    "--face-model-dir <dir>",
    "Local directory with a previously downloaded face detection model, for fully offline use (see docs/FACE_CROP.md)",
  )
  .action(async (input, options) => {
    let outputDir;
    let useTmpDir = false;

    // Fetch device settings if --device-parameters is specified
    let deviceSettings = null;
    let devicePalette = null;
    if (options.deviceParameters) {
      try {
        deviceSettings = await fetchDeviceSettings(options.host);
        devicePalette = await fetchDevicePalette(options.host);
        // Fetch orientation from device (overrides --orientation flag)
        options.orientation = await fetchDeviceOrientation(options.host);
      } catch (error) {
        console.error(`Error: ${error.message}`);
        process.exit(1);
      }
    }

    try {
      const inputPath = path.resolve(input);
      if (!fs.existsSync(inputPath)) {
        console.error(`Error: Input path not found: ${inputPath}`);
        process.exit(1);
      }

      // Parse dimension if provided
      const dimensionExplicit = program.getOptionValueSource("dimension") === "cli";
      if (options.dimension) {
        const match = options.dimension.match(/^(\d+)x(\d+)$/);
        if (match) {
          options.displayWidth = parseInt(match[1]);
          options.displayHeight = parseInt(match[2]);
        } else {
          console.error(
            `Error: Invalid dimension format "${options.dimension}". Use WxH (e.g. 800x480)`,
          );
          process.exit(1);
        }
      }

      // --resolution is an alias of --dimension (also the face-crop target's
      // pixel size) - parsed the same way, applied after --dimension so it
      // wins if both happen to be given.
      const resolutionExplicit = program.getOptionValueSource("resolution") === "cli";
      if (options.resolution) {
        const match = options.resolution.match(/^(\d+)x(\d+)$/);
        if (match) {
          options.displayWidth = parseInt(match[1]);
          options.displayHeight = parseInt(match[2]);
        } else {
          console.error(
            `Error: Invalid resolution format "${options.resolution}". Use WxH (e.g. 800x480)`,
          );
          process.exit(1);
        }
      }

      // --board provides default display dimensions, but only when no more
      // specific sizing flag was explicitly given (--resolution/--dimension/
      // --display-width/--display-height all take precedence).
      if (options.board && !dimensionExplicit && !resolutionExplicit) {
        const displayWidthExplicit = program.getOptionValueSource("displayWidth") === "cli";
        const displayHeightExplicit = program.getOptionValueSource("displayHeight") === "cli";
        if (!displayWidthExplicit && !displayHeightExplicit) {
          const profile = getBoardProfile(options.board);
          if (!profile) {
            console.error(`Error: Unknown --board "${options.board}"`);
            process.exit(1);
          }
          options.displayWidth = profile.width;
          options.displayHeight = profile.height;
        }
      }

      // If --host is explicitly specified, query device for display resolution
      // and firmware version (to determine output format)
      // This overwrites any -d / --display-width / --display-height values
      let deviceVersion = "";
      const hostExplicit = program.getOptionValueSource("host") === "cli";
      if (hostExplicit) {
        try {
          const sysInfo = await fetchDeviceSystemInfo(options.host);
          options.displayWidth = sysInfo.width;
          options.displayHeight = sysInfo.height;
          deviceVersion = sysInfo.version;
        } catch (error) {
          console.error(
            `Warning: Could not fetch system info from device: ${error.message}`,
          );
          console.error(
            `  Using default resolution: ${options.displayWidth}x${options.displayHeight}`,
          );
        }
      }

      // Auto-select format based on firmware version when uploading/displaying
      // to device and format is not explicitly overridden by user
      const formatExplicit = program.getOptionValueSource("format") === "cli";
      if (!formatExplicit && (options.upload || options.direct)) {
        if (!supportsEPDGZ(deviceVersion)) {
          console.log(
            `Device firmware ${deviceVersion || "(unknown)"} does not support epdgz, using PNG`,
          );
          options.format = "png";
        }
      }

      // Apply preset values if not overridden by explicit options
      const presetName = options.preset || "balanced";
      // presetParams is declared here, ensure no duplicate declaration below
      const presetParams = getPreset(presetName) || {};

      // Helper to set option if not defined, strictly checking undefined
      // so that 0 is treated as a valid value
      const setIfNotDefined = (key, value) => {
        if (options[key] === undefined && value !== undefined) {
          options[key] = value;
        }
      };

      // Set defaults from preset
      setIfNotDefined("exposure", presetParams.exposure);
      setIfNotDefined("saturation", presetParams.saturation);
      setIfNotDefined("toneMode", presetParams.toneMode);
      setIfNotDefined("contrast", presetParams.contrast);
      // CLI declares --scurve-strength etc., which Commander maps to
      // options.scurveStrength. Write preset/default fallbacks into the same
      // property so there's one source of truth per scurve parameter.
      setIfNotDefined("scurveStrength", presetParams.strength);
      setIfNotDefined("scurveShadow", presetParams.shadowBoost);
      setIfNotDefined("scurveHighlight", presetParams.highlightCompress);
      setIfNotDefined("scurveMidpoint", presetParams.midpoint);
      setIfNotDefined("colorMethod", presetParams.colorMethod);
      setIfNotDefined("ditherAlgorithm", presetParams.ditherAlgorithm);
      setIfNotDefined(
        "compressDynamicRange",
        presetParams.compressDynamicRange,
      );

      // Set fallback defaults if still undefined (from global defaults)
      const libraryDefaults = getDefaultParams();
      setIfNotDefined("exposure", libraryDefaults.exposure);
      setIfNotDefined("saturation", libraryDefaults.saturation);
      setIfNotDefined("toneMode", libraryDefaults.toneMode);
      setIfNotDefined("contrast", libraryDefaults.contrast);
      setIfNotDefined("scurveStrength", libraryDefaults.strength);
      setIfNotDefined("scurveShadow", libraryDefaults.shadowBoost);
      setIfNotDefined("scurveHighlight", libraryDefaults.highlightCompress);
      setIfNotDefined("scurveMidpoint", libraryDefaults.midpoint);
      setIfNotDefined("colorMethod", libraryDefaults.colorMethod);
      setIfNotDefined("ditherAlgorithm", libraryDefaults.ditherAlgorithm);
      setIfNotDefined(
        "compressDynamicRange",
        libraryDefaults.compressDynamicRange,
      );

      if (!options.silent) {
        console.log(`Using preset: ${presetName}`);
      }

      // Build processing options with priority: device settings > CLI options > preset > defaults
      // When using device settings, they take full priority
      // Otherwise: user CLI options override preset values, preset values override defaults
      const processOptions = deviceSettings
        ? {
            generateThumbnail: true,
            exposure: deviceSettings.exposure,
            saturation: deviceSettings.saturation,
            toneMode: deviceSettings.toneMode,
            contrast: deviceSettings.contrast,
            strength: deviceSettings.strength,
            shadowBoost: deviceSettings.shadowBoost,
            highlightCompress: deviceSettings.highlightCompress,
            midpoint: deviceSettings.midpoint,
            colorMethod: deviceSettings.colorMethod,
            usePerceivedOutput: options.usePerceivedOutput || false,
            ditherAlgorithm:
              deviceSettings.ditherAlgorithm || options.ditherAlgorithm,
            compressDynamicRange: deviceSettings.compressDynamicRange,
            displayWidth: options.displayWidth,
            displayHeight: options.displayHeight,
            format: options.format,
            autoOrient: options.autoOrient || false,
            orientation: options.orientation || "landscape",
            scaleMode: deviceSettings.scaleMode || options.scaleMode || "cover",
            backgroundColor:
              deviceSettings.backgroundColor ||
              options.backgroundColor ||
              "white",
            grayscale: options.grayscale || false,
          }
        : {
            generateThumbnail: true,
            // User CLI options override preset values
            exposure:
              options.exposure ??
              presetParams?.exposure ??
              DEFAULT_PARAMS.exposure,
            saturation:
              options.saturation ??
              presetParams?.saturation ??
              DEFAULT_PARAMS.saturation,
            toneMode:
              options.toneMode ??
              presetParams?.toneMode ??
              DEFAULT_PARAMS.toneMode,
            contrast:
              options.contrast ??
              presetParams?.contrast ??
              DEFAULT_PARAMS.contrast,
            // --scurve-* CLI flags land in options.scurve*; setIfNotDefined
            // above filled in preset/library defaults on the same property.
            // The output keys (strength etc.) match what the library reads.
            strength:
              options.scurveStrength ??
              presetParams?.strength ??
              DEFAULT_PARAMS.strength,
            shadowBoost:
              options.scurveShadow ??
              presetParams?.shadowBoost ??
              DEFAULT_PARAMS.shadowBoost,
            highlightCompress:
              options.scurveHighlight ??
              presetParams?.highlightCompress ??
              DEFAULT_PARAMS.highlightCompress,
            midpoint:
              options.scurveMidpoint ??
              presetParams?.midpoint ??
              DEFAULT_PARAMS.midpoint,
            colorMethod:
              options.colorMethod ??
              presetParams?.colorMethod ??
              DEFAULT_PARAMS.colorMethod,
            usePerceivedOutput: options.usePerceivedOutput ?? false,
            ditherAlgorithm:
              options.ditherAlgorithm ??
              presetParams?.ditherAlgorithm ??
              DEFAULT_PARAMS.ditherAlgorithm,
            compressDynamicRange:
              options.compressDynamicRange ??
              presetParams?.compressDynamicRange ??
              DEFAULT_PARAMS.compressDynamicRange,
            displayWidth: options.displayWidth,
            displayHeight: options.displayHeight,
            format: options.format,
            autoOrient: options.autoOrient || false,
            orientation: options.orientation || "landscape",
            scaleMode: options.scaleMode || "cover",
            backgroundColor: options.backgroundColor || "white",
            grayscale: options.grayscale || false,
          };

      // Face-aware crop metadata setup (opt-in via --detect-faces). Loading
      // the detector happens once here, before any per-image work, so a
      // batch run only pays the model-load cost once.
      let faceCropContext = null;
      if (options.metadataOnly && !options.detectFaces) {
        console.error("Error: --metadata-only requires --detect-faces");
        process.exit(1);
      }
      const cropOutputExplicit = program.getOptionValueSource("cropOutput") === "cli";
      if (cropOutputExplicit && !options.detectFaces) {
        console.error("Error: --crop-output requires --detect-faces");
        process.exit(1);
      }
      if (!["cropped", "uncropped", "both"].includes(options.cropOutput)) {
        console.error(
          `Error: Invalid --crop-output "${options.cropOutput}". Use 'cropped', 'uncropped', or 'both'`,
        );
        process.exit(1);
      }
      if (cropOutputExplicit && options.metadataOnly) {
        console.warn(
          "Warning: --crop-output is ignored because --metadata-only skips all rendered images",
        );
      }
      if (options.detectFaces) {
        let target;
        try {
          const orientationExplicit = program.getOptionValueSource("orientation") === "cli";
          target = normalizeTargetGeometry({
            board: options.board,
            resolution: options.resolution,
            displaySizeMm: options.displaySizeMm,
            orientation: orientationExplicit ? options.orientation : "auto",
            fallbackWidth: options.displayWidth,
            fallbackHeight: options.displayHeight,
          });
        } catch (error) {
          console.error(`Error: ${error.message}`);
          process.exit(1);
        }
        for (const warning of target.warnings) {
          console.warn(`Warning: ${warning}`);
        }
        console.log(
          `Face-aware crop target: ${target.width}x${target.height} (${target.orientation})` +
            (target.board ? `, board=${target.board}` : ""),
        );

        console.log("Loading face detection model (blazeface)...");
        const detector = await getFaceDetector("blazeface", {
          scoreThreshold: options.faceMinScore,
          modelDir: options.faceModelDir,
        });
        console.log("Face detection model ready");

        faceCropContext = {
          enabled: true,
          metadataOnly: !!options.metadataOnly,
          cropOutput: options.cropOutput,
          target,
          marginPercent: options.faceMargin,
          detector,
          engineName: "blazeface",
        };
      }
      processOptions.faceCrop = faceCropContext;

      // --crop-output both produces two images (<name>.cover.<ext> and
      // <name>.fit.<ext>) - --upload/--direct only ever send one image, so
      // reject the ambiguous combination up front rather than silently
      // picking one.
      if (
        faceCropContext &&
        !faceCropContext.metadataOnly &&
        faceCropContext.cropOutput === "both" &&
        (options.upload || options.direct)
      ) {
        console.error(
          "Error: --crop-output both produces two images and can't be used with --upload/--direct " +
            "(process to disk with -o instead, then upload the file you want manually)",
        );
        process.exit(1);
      }

      // Check if --serve mode is enabled
      if (options.serve) {
        const inputStats = fs.statSync(inputPath);
        if (!inputStats.isDirectory()) {
          console.error(
            "Error: --serve mode requires a directory with album structure",
          );
          process.exit(1);
        }

        const port = parseInt(options.servePort);
        if (isNaN(port) || port < 1 || port > 65535) {
          console.error(`Error: Invalid port number: ${options.servePort}`);
          process.exit(1);
        }

        // Start HTTP server and keep running
        await createImageServer(
          inputPath,
          port,
          options.serveFormat,
          devicePalette,
          processOptions, // Pass resolved options
          options,
        );
        return; // Server runs indefinitely
      }

      // Validate --direct and --upload are mutually exclusive
      if (options.direct && options.upload) {
        console.error("Error: --direct and --upload cannot be used together");
        process.exit(1);
      }

      // Check input type and validate --direct option
      const inputStats = fs.statSync(inputPath);
      const isDirectory = inputStats.isDirectory();

      // Validate --direct requires single file
      if (options.direct && isDirectory) {
        console.error(
          "Error: --direct option requires a single file input, not a directory",
        );
        process.exit(1);
      }

      if (options.upload || options.direct) {
        if (faceCropContext) {
          console.warn(
            "Warning: --detect-faces metadata is written into the temporary directory used by " +
              "--upload/--direct and will be deleted with it afterward, not uploaded to the device",
          );
        }
        outputDir = fs.mkdtempSync(path.join(os.tmpdir(), "photoframe-"));
        useTmpDir = true;
        console.log(`Using temporary directory: ${outputDir}`);
      } else {
        outputDir = path.resolve(options.outputDir);
        if (!fs.existsSync(outputDir)) {
          fs.mkdirSync(outputDir, { recursive: true });
        }
      }

      // Explicit preset option validation
      if (options.preset) {
        if (!getPreset(options.preset)) {
          console.error(`Error: Unknown preset "${options.preset}"`);
          console.error(`Available presets: ${getPresetNames().join(", ")}`);
          process.exit(1);
        }
      }

      // Process based on input type
      if (isDirectory) {
        // Process folder structure (albums)
        await processFolderStructure(
          inputPath,
          outputDir,
          processOptions,
          devicePalette,
          options.upload ? options.host : null,
        );
      } else {
        // Process single file
        const baseName = path.basename(input, path.extname(input));
        const suffix = options.suffix || "";
        const format = processOptions.format || "epdgz";
        const ext =
          format === "bmp" ? ".bmp" : format === "png" ? ".png" : ".epdgz";
        const outputBasePath = path.join(outputDir, `${baseName}${suffix}`);

        const rendered = await processImageFile(
          inputPath,
          outputBasePath,
          ext,
          processOptions,
          devicePalette,
        );

        // Upload or display directly on device
        if (options.upload || options.direct) {
          if (format === "bmp") {
            console.error(
              `Error: Upload/direct display does not support BMP format`,
            );
            process.exit(1);
          }
          if (rendered.length === 0) {
            console.error(
              `Error: --metadata-only produced no image to ${options.direct ? "display" : "upload"}`,
            );
            process.exit(1);
          }
          // --crop-output both was already rejected together with --upload/
          // --direct earlier, so exactly one variant exists here.
          const { outputFile, outputThumb } = rendered[0];
          if (!fs.existsSync(outputFile)) {
            console.error(`Error: Output file not found: ${outputFile}`);
            process.exit(1);
          }
          if (!fs.existsSync(outputThumb)) {
            console.error(`Error: Thumbnail file not found: ${outputThumb}`);
            process.exit(1);
          }

          try {
            if (options.direct) {
              await displayDirectly(options.host, outputFile, outputThumb);
            } else {
              await uploadToDevice(options.host, outputFile, outputThumb, null);
            }
          } catch (error) {
            const action = options.direct ? "Display" : "Upload";
            console.error(`${action} failed: ${error.message}`);
            process.exit(1);
          }
        }
      }

      // Cleanup temporary directory if used
      if (useTmpDir) {
        console.log(`\nCleaning up temporary directory: ${outputDir}`);
        try {
          fs.rmSync(outputDir, { recursive: true, force: true });
          console.log(`✓ Temporary files cleaned up`);
        } catch (error) {
          console.warn(
            `Warning: Failed to cleanup temporary directory: ${error.message}`,
          );
        }
      }
    } catch (error) {
      console.error(`Error processing: ${error.message}`);
      console.error(error.stack);

      // Cleanup temporary directory on error if used
      if (useTmpDir && outputDir) {
        try {
          fs.rmSync(outputDir, { recursive: true, force: true });
        } catch (cleanupError) {
          // Ignore cleanup errors on error path
        }
      }

      process.exit(1);
    }
  });

program.parse();
