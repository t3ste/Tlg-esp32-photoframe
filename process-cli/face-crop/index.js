/**
 * Face-aware crop metadata - public entry point.
 *
 * See docs/FACE_CROP.md for the feature overview, JSON schema, and the
 * planned firmware-side consumption flow.
 */

import { registerFaceDetector, createFaceDetector, listFaceDetectors } from "./detector.js";
import { createBlazefaceDetector } from "./blazeface-adapter.js";
import {
  normalizeTargetGeometry,
  getBoardProfiles,
  getBoardProfile,
  parseWidthHeight,
  orientationFromDims,
} from "./geometry.js";
import {
  computeRecommendedCrop,
  fallbackCrop,
  clampCropToImage,
  scoreFaces,
} from "./crop-engine.js";
import {
  buildMetadata,
  writeMetadataFile,
  readMetadataFile,
  metadataPathFor,
  FACECROP_SCHEMA_VERSION,
} from "./metadata.js";

registerFaceDetector("blazeface", createBlazefaceDetector);

export {
  normalizeTargetGeometry,
  getBoardProfiles,
  getBoardProfile,
  parseWidthHeight,
  orientationFromDims,
  computeRecommendedCrop,
  fallbackCrop,
  clampCropToImage,
  scoreFaces,
  buildMetadata,
  writeMetadataFile,
  readMetadataFile,
  metadataPathFor,
  FACECROP_SCHEMA_VERSION,
  createFaceDetector,
  listFaceDetectors,
};

// One loaded detector instance per (engine name + options) combination,
// reused across every image in a batch run so the model is only loaded
// once per CLI invocation, not once per photo.
const detectorCache = new Map();

export async function getFaceDetector(name, options = {}) {
  const key = `${name}:${JSON.stringify(options)}`;
  if (!detectorCache.has(key)) {
    const detector = createFaceDetector(name, options);
    await detector.load();
    detectorCache.set(key, detector);
  }
  return detectorCache.get(key);
}

/**
 * Detects faces on an already-oriented image and computes the recommended
 * crop for `target`.
 *
 * @param {Object} params
 * @param {import('./detector.js').FaceDetector} params.detector - Loaded detector.
 * @param {{data: Uint8ClampedArray, width: number, height: number}} params.imageData
 * @param {{width:number,height:number,aspectRatio:number,orientation:string,board?:string}} params.target
 * @param {number} [params.marginPercent=0.12]
 * @param {string} params.engineName - For the metadata `strategy.engine` field.
 * @returns {Promise<{faces: Array, recommendedCrop: Object, strategy: Object}>}
 */
export async function analyzeFaceCrop({
  detector,
  imageData,
  target,
  marginPercent = 0.12,
  engineName,
}) {
  const faces = await detector.detect(imageData);
  const recommendedCrop = computeRecommendedCrop(
    imageData.width,
    imageData.height,
    faces,
    target,
    { marginPercent },
  );
  return {
    faces,
    recommendedCrop,
    strategy: {
      mode: faces.length > 0 ? "largest-face-priority" : "center-crop-fallback",
      marginPercent,
      engine: engineName,
    },
  };
}
