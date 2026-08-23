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
 * @param {number} [options.iouThreshold=0.3]
 * @param {string} [options.modelDir] - Local directory with a previously
 *   downloaded model.json + shards, for fully offline use.
 * @returns {import('./detector.js').FaceDetector}
 */
export function createBlazefaceDetector(options = {}) {
  const { maxFaces = 10, scoreThreshold = 0.75, iouThreshold = 0.3, modelDir = null } = options;

  let model = null;

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
        return { x: x0, y: y0, w: x1 - x0, h: y1 - y0, score };
      });
    },

    dispose() {
      model = null;
    },
  };
}
