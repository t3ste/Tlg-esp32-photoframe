/**
 * Versioned per-image face-crop metadata (`<basename>.facecrop.json`).
 *
 * One file per source image rather than a single global index - robust
 * against files being moved/copied independently onto an SD card, trivial
 * for the firmware to associate with its source image, no database to keep
 * in sync. See docs/FACE_CROP.md for the full schema description and the
 * planned firmware-side consumption flow.
 */

import fs from "fs";
import path from "path";

export const FACECROP_SCHEMA_VERSION = 1;
export const FACECROP_SUFFIX = ".facecrop.json";

/** Derives the metadata sidecar path for a given output image path. */
export function metadataPathFor(outputImagePath) {
  const dir = path.dirname(outputImagePath);
  const base = path.basename(outputImagePath, path.extname(outputImagePath));
  return path.join(dir, `${base}${FACECROP_SUFFIX}`);
}

/**
 * Builds the metadata object (not yet written to disk) for one processed
 * image.
 *
 * @param {Object} params
 * @param {string} params.sourcePath - Original input file path (or basename).
 * @param {{width:number,height:number}} params.image - Dimensions of the
 *   upright (EXIF-corrected, orientation-matched) image that `faces` and
 *   `recommendedCrop` are measured against.
 * @param {{board?:string,width:number,height:number,aspectRatio:number,orientation:string}} params.target
 * @param {Array<{x:number,y:number,w:number,h:number,score:number}>} params.faces
 * @param {{x:number,y:number,w:number,h:number}} params.recommendedCrop
 * @param {{mode:string,marginPercent:number,engine:string}} params.strategy
 * @param {string} params.generatorVersion
 */
export function buildMetadata({
  sourcePath,
  image,
  target,
  faces,
  recommendedCrop,
  strategy,
  generatorVersion,
}) {
  return {
    schema: FACECROP_SCHEMA_VERSION,
    source: sourcePath,
    image: { width: image.width, height: image.height },
    target: {
      board: target.board,
      width: target.width,
      height: target.height,
      aspect_ratio: Math.round(target.aspectRatio * 10000) / 10000,
      orientation: target.orientation,
    },
    faces: faces.map((f) => ({
      x: Math.round(f.x),
      y: Math.round(f.y),
      w: Math.round(f.w),
      h: Math.round(f.h),
      score: Math.round((f.score ?? 0) * 10000) / 10000,
    })),
    recommended_crop: {
      x: Math.round(recommendedCrop.x),
      y: Math.round(recommendedCrop.y),
      w: Math.round(recommendedCrop.w),
      h: Math.round(recommendedCrop.h),
    },
    strategy: {
      mode: strategy.mode,
      margin_percent: strategy.marginPercent,
      engine: strategy.engine,
    },
    timestamp: new Date().toISOString(),
    generator_version: generatorVersion,
  };
}

export function writeMetadataFile(metadataPath, metadata) {
  fs.writeFileSync(metadataPath, JSON.stringify(metadata, null, 2) + "\n");
}

/** Reads and parses a facecrop.json file. Throws if missing/invalid/unsupported schema. */
export function readMetadataFile(metadataPath) {
  const raw = fs.readFileSync(metadataPath, "utf8");
  const metadata = JSON.parse(raw);
  if (metadata.schema !== FACECROP_SCHEMA_VERSION) {
    throw new Error(
      `Unsupported facecrop schema version ${metadata.schema} in ${metadataPath} ` +
        `(expected ${FACECROP_SCHEMA_VERSION})`,
    );
  }
  return metadata;
}
