/**
 * Face detection adapter interface + registry.
 *
 * The crop engine and CLI only ever talk to this interface, never to a
 * specific ML library - see docs/FACE_CROP.md for why (BlazeFace today,
 * swappable later without touching crop-engine.js or cli.js).
 *
 * @typedef {Object} DetectedFace
 * @property {number} x
 * @property {number} y
 * @property {number} w
 * @property {number} h
 * @property {number} score
 *
 * @typedef {Object} FaceDetector
 * @property {() => Promise<void>} load
 * @property {(imageData: {data: Uint8ClampedArray|Buffer, width: number, height: number}) => Promise<DetectedFace[]>} detect
 * @property {() => void} [dispose]
 */

const registry = new Map();

/** Registers a detector factory under `name` (e.g. "blazeface"). */
export function registerFaceDetector(name, factory) {
  registry.set(name, factory);
}

export function listFaceDetectors() {
  return [...registry.keys()];
}

/** Instantiates (but does not `load()`) the named detector. */
export function createFaceDetector(name, options = {}) {
  const factory = registry.get(name);
  if (!factory) {
    throw new Error(
      `Unknown face detection engine "${name}". Available: ${listFaceDetectors().join(", ") || "(none registered)"}`,
    );
  }
  return factory(options);
}
