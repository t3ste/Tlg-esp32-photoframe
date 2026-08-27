/**
 * BlazeFace adapter (implements the FaceDetector interface in detector.js).
 *
 * Uses @tensorflow/tfjs-core + tfjs-converter + tfjs-backend-cpu directly
 * (not the full @tensorflow/tfjs umbrella package, and not @tensorflow/tfjs-node)
 * so there's no native addon to compile and no GPU/browser-only backend
 * dragged in - a pure-JS CPU backend that runs unmodified on any platform
 * Node itself supports. Slower than tfjs-node per image, which is an
 * acceptable trade for a batch CLI tool that prioritizes "just works on
 * Windows and Linux" over raw throughput. See docs/FACE_CROP.md.
 *
 * Faces are detected against a tensor built directly from already-decoded
 * RGBA pixel data (from node-canvas's getImageData) - deliberately not via
 * tf.browser.fromPixels(), which requires a DOM/tfjs-node canvas polyfill
 * neither of which is available in a plain Node + tfjs-core setup.
 *
 * Small/distant faces: the downloaded BlazeFace graph has a frozen 128x128x3
 * input - confirmed directly (passing a larger inputWidth/inputHeight to
 * blazeface.load() resizes the image before feeding it in, but the graph
 * itself then rejects any shape other than exactly 128x128 with a hard
 * error), so it isn't a usable knob for this model. Whatever the source
 * photo's resolution, every face gets squashed down to whatever it looks
 * like inside a 128x128 image - a face that's a small fraction of a
 * multi-megapixel photo can shrink to just a few pixels and vanish
 * entirely, regardless of --face-min-score (there's no candidate box to
 * threshold in the first place). The fix is tiling: split the image into
 * overlapping sub-regions and run the same 128x128-limited detector on each
 * one, so a small face becomes a much larger fraction of whatever tile it
 * falls in. See tileGrid below - confirmed on a real 5-person photo where
 * whole-image detection found only the 3 largest/closest faces: a 3x3 tile
 * grid found the other 2 (a sunglasses-occluded face at score 0.98 and a
 * distant, otherwise perfectly clear one at score 0.77-0.98 depending on
 * which tile it fell in) without any change to --face-min-score.
 */

import * as tf from "@tensorflow/tfjs-core";
import "@tensorflow/tfjs-backend-cpu";
import "@tensorflow/tfjs-converter";
import blazeface from "@tensorflow-models/blazeface";
import fs from "fs";
import path from "path";

/**
 * Builds a tfjs IOHandler that reads a previously-downloaded graph model
 * (a model.json plus its weight shard file(s)) from a local directory, so
 * the model can be reused fully offline after being fetched once. Users can
 * populate `modelDir` by downloading the files listed at
 * https://tfhub.dev/tensorflow/tfjs-model/blazeface/1/default/1 - see
 * docs/FACE_CROP.md for exact commands.
 */
function localDirIOHandler(modelDir) {
  return {
    load: async () => {
      const modelJsonPath = path.join(modelDir, "model.json");
      const modelJson = JSON.parse(fs.readFileSync(modelJsonPath, "utf8"));
      const weightsManifest = modelJson.weightsManifest;
      if (!weightsManifest) {
        throw new Error(`${modelJsonPath} has no weightsManifest`);
      }

      const weightSpecs = [];
      const buffers = [];
      for (const group of weightsManifest) {
        weightSpecs.push(...group.weights);
        for (const shardPath of group.paths) {
          buffers.push(fs.readFileSync(path.join(modelDir, shardPath)));
        }
      }
      const weightData = Buffer.concat(buffers).buffer;

      return {
        modelTopology: modelJson.modelTopology,
        weightSpecs,
        weightData,
        format: modelJson.format,
        generatedBy: modelJson.generatedBy,
        convertedBy: modelJson.convertedBy,
        signature: modelJson.signature,
        userDefinedMetadata: modelJson.userDefinedMetadata,
      };
    },
  };
}

/**
 * @param {Object} [options]
 * @param {number} [options.maxFaces=10]
 * @param {number} [options.scoreThreshold=0.75] - Minimum confidence to keep a face.
 * @param {number} [options.iouThreshold=0.3] - Also reused as the dedup threshold when
 *   merging tile detections back together (see tileGrid below).
 * @param {string} [options.modelDir] - Local directory with a previously
 *   downloaded model.json + shards, for fully offline use.
 * @param {number} [options.tileGrid=1] - Split each image into an NxN grid of overlapping
 *   tiles and run detection on each in addition to the whole image, to catch small/distant
 *   faces (see the module doc comment above for why). 1 = disabled (whole-image detection
 *   only, the original behavior).
 * @param {number} [options.tileOverlap=0.2] - Fractional overlap between adjacent tiles
 *   (of the tile's own size), so a face sitting across a tile boundary still lands fully
 *   inside at least one tile.
 * @returns {import('./detector.js').FaceDetector}
 */
export function createBlazefaceDetector(options = {}) {
  const {
    maxFaces = 10,
    scoreThreshold = 0.75,
    iouThreshold = 0.3,
    modelDir = null,
    tileGrid = 1,
    tileOverlap = 0.2,
  } = options;

  let model = null;

  async function detectRegion(imageData, offsetX, offsetY) {
    const { data, width, height } = imageData;
    // Drop the alpha channel - blazeface only wants RGB.
    const rgb = new Int32Array(width * height * 3);
    for (let i = 0, j = 0; i < data.length; i += 4, j += 3) {
      rgb[j] = data[i];
      rgb[j + 1] = data[i + 1];
      rgb[j + 2] = data[i + 2];
    }

    const input = tf.tensor3d(rgb, [height, width, 3], "int32");
    let predictions;
    try {
      predictions = await model.estimateFaces(input, false);
    } finally {
      input.dispose();
    }

    return predictions.map((p) => {
      const [x0, y0] = p.topLeft;
      const [x1, y1] = p.bottomRight;
      const score = p.probability != null ? p.probability[0] : 0;
      return { x: x0 + offsetX, y: y0 + offsetY, w: x1 - x0, h: y1 - y0, score };
    });
  }

  return {
    async load() {
      await tf.setBackend("cpu");
      await tf.ready();
      const modelUrl = modelDir ? localDirIOHandler(modelDir) : undefined;
      model = await blazeface.load({ maxFaces, scoreThreshold, iouThreshold, modelUrl });
    },

    async detect(imageData) {
      if (!model) {
        throw new Error("BlazeFace detector used before load()");
      }

      const fullFrameFaces = await detectRegion(imageData, 0, 0);
      if (tileGrid <= 1) {
        return fullFrameFaces;
      }

      const tiles = computeTiles(imageData.width, imageData.height, tileGrid, tileOverlap);
      let tileFaces = [];
      for (const tile of tiles) {
        const region = cropImageData(imageData, tile.x, tile.y, tile.w, tile.h);
        tileFaces = tileFaces.concat(await detectRegion(region, tile.x, tile.y));
      }

      return mergeOverlappingDetections([...fullFrameFaces, ...tileFaces], iouThreshold);
    },

    dispose() {
      model = null;
    },
  };
}

/**
 * Lays out an NxN grid of overlapping tiles covering the full imgWidth x imgHeight
 * area, each sized so that gridSize tiles with `overlap` fractional overlap between
 * neighbors exactly span the image. Pure geometry - see test/face-crop-tiling.test.js.
 *
 * @returns {Array<{x:number,y:number,w:number,h:number}>}
 */
export function computeTiles(imgWidth, imgHeight, gridSize, overlap = 0.2) {
  const tileW = Math.min(imgWidth, Math.ceil(imgWidth / gridSize / (1 - overlap)));
  const tileH = Math.min(imgHeight, Math.ceil(imgHeight / gridSize / (1 - overlap)));
  const stepX = gridSize > 1 ? (imgWidth - tileW) / (gridSize - 1) : 0;
  const stepY = gridSize > 1 ? (imgHeight - tileH) / (gridSize - 1) : 0;

  const tiles = [];
  for (let row = 0; row < gridSize; row++) {
    for (let col = 0; col < gridSize; col++) {
      tiles.push({
        x: Math.round(col * stepX),
        y: Math.round(row * stepY),
        w: tileW,
        h: tileH,
      });
    }
  }
  return tiles;
}

/** Extracts a sub-rectangle of an RGBA ImageData-shaped object as a new one. */
export function cropImageData(imageData, x, y, w, h) {
  const { data, width } = imageData;
  const out = new Uint8ClampedArray(w * h * 4);
  for (let row = 0; row < h; row++) {
    const srcStart = ((y + row) * width + x) * 4;
    out.set(data.subarray(srcStart, srcStart + w * 4), row * w * 4);
  }
  return { data: out, width: w, height: h };
}

function iou(a, b) {
  const x0 = Math.max(a.x, b.x);
  const y0 = Math.max(a.y, b.y);
  const x1 = Math.min(a.x + a.w, b.x + b.w);
  const y1 = Math.min(a.y + a.h, b.y + b.h);
  const interArea = Math.max(0, x1 - x0) * Math.max(0, y1 - y0);
  const unionArea = a.w * a.h + b.w * b.h - interArea;
  return unionArea > 0 ? interArea / unionArea : 0;
}

/**
 * Deduplicates the same physical face detected multiple times (once per
 * whole-image pass and once per overlapping tile it happened to fall in) via
 * greedy non-max suppression: highest score wins, anything overlapping it by
 * more than `iouThreshold` is dropped.
 */
export function mergeOverlappingDetections(faces, iouThreshold = 0.3) {
  const sorted = [...faces].sort((a, b) => b.score - a.score);
  const kept = [];
  for (const face of sorted) {
    if (!kept.some((k) => iou(k, face) > iouThreshold)) {
      kept.push(face);
    }
  }
  return kept;
}
