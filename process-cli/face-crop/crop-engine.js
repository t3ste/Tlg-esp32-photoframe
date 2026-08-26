/**
 * Face-aware crop recommendation.
 *
 * Pure geometry - no image decoding or ML inference here, so this is cheap
 * to unit test with hand-built face lists. Faces and crops are always
 * expressed in source-image pixel space (whatever coordinate space the
 * caller's detector ran against).
 */

/** Sorts faces by bounding-box area, largest first. Adds an `area` field. */
export function scoreFaces(faces) {
  return faces
    .map((f) => ({ ...f, area: f.w * f.h }))
    .sort((a, b) => b.area - a.area);
}

function expandWithMargin(face, marginPercent) {
  const mx = face.w * marginPercent;
  const my = face.h * marginPercent;
  return { x: face.x - mx, y: face.y - my, w: face.w + 2 * mx, h: face.h + 2 * my };
}

function union(a, b) {
  const x0 = Math.min(a.x, b.x);
  const y0 = Math.min(a.y, b.y);
  const x1 = Math.max(a.x + a.w, b.x + b.w);
  const y1 = Math.max(a.y + a.h, b.y + b.h);
  return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
}

// Allows sub-pixel rounding slop when checking containment.
const EPS = 0.5;
function contains(outer, inner) {
  return (
    inner.x >= outer.x - EPS &&
    inner.y >= outer.y - EPS &&
    inner.x + inner.w <= outer.x + outer.w + EPS &&
    inner.y + inner.h <= outer.y + outer.h + EPS
  );
}

/** Grows a box to match aspectRatio (width/height), keeping it centered. Never shrinks. */
function growToAspect(box, aspectRatio) {
  let { x, y, w, h } = box;
  const currentAspect = w / h;
  if (currentAspect < aspectRatio) {
    const newW = h * aspectRatio;
    const cx = x + w / 2;
    x = cx - newW / 2;
    w = newW;
  } else if (currentAspect > aspectRatio) {
    const newH = w / aspectRatio;
    const cy = y + h / 2;
    y = cy - newH / 2;
    h = newH;
  }
  return { x, y, w, h };
}

/** The largest {w,h} of aspectRatio that fits entirely within the image. */
function maxBoxForAspect(imgWidth, imgHeight, aspectRatio) {
  if (imgWidth / imgHeight > aspectRatio) {
    const h = imgHeight;
    return { w: h * aspectRatio, h };
  }
  const w = imgWidth;
  return { w, h: w / aspectRatio };
}

/**
 * Grows `box` to aspectRatio like growToAspect(), but - when the source
 * image has room to spare - expands further to use as much of it as
 * possible, rather than only the minimal amount growToAspect() alone would
 * add. E.g. a source image much wider than the target aspect ratio only
 * needs its height cropped down to reach it; growToAspect() alone would
 * still leave the crop only as wide as `box` already was, discarding both
 * sides of the image for no reason. Confirmed on real output: a 3-face
 * photo wider than 5:3 had ~600px trimmed off its left/right edges even
 * though cropping only its height (no left/right loss at all) would have
 * fit the same 3 faces just as well.
 *
 * Falls back to the plain growToAspect() box when the image itself isn't
 * big enough to contain `box` at the target aspect ratio without clipping
 * it (i.e. `box` genuinely needs more width or height than fits, so no
 * bigger box centered on it can help).
 */
function growToFillImage(box, imgWidth, imgHeight, aspectRatio) {
  const minimal = growToAspect(box, aspectRatio);
  const { w: maxW, h: maxH } = maxBoxForAspect(imgWidth, imgHeight, aspectRatio);

  if (maxW <= minimal.w || maxH <= minimal.h) {
    return minimal;
  }

  const cx = box.x + box.w / 2;
  const cy = box.y + box.h / 2;
  let x = cx - maxW / 2;
  let y = cy - maxH / 2;
  x = Math.max(0, Math.min(x, imgWidth - maxW));
  y = Math.max(0, Math.min(y, imgHeight - maxH));
  const maximal = { x, y, w: maxW, h: maxH };

  return contains(maximal, box) ? maximal : minimal;
}

/**
 * Clamps a crop rectangle to lie fully within [0,imgW] x [0,imgH], preserving
 * its aspect ratio (uniform scale-down if it's bigger than the image in
 * either dimension, then pure translation to bring it back in bounds).
 *
 * Deliberately returns full sub-pixel precision, not rounded to whole
 * pixels - this is also used internally (computeRecommendedCrop's per-face
 * "does it still fit" check) to build the box a containment test is run
 * against. Rounding x/y/w/h independently here can shave up to ~1px total
 * off the effective right/bottom edge (e.g. x rounds down 0.3px *and* w
 * rounds down 0.5px), which the containment check's own EPS=0.5 tolerance
 * doesn't always absorb - confirmed on real output: a face that
 * legitimately fit was rejected this way, needlessly shrinking the final
 * crop to exclude it. Callers that need whole-pixel output (the final
 * recommended crop, ultimately used to size an actual pixel buffer) round
 * once at their own final return - see computeRecommendedCrop/fallbackCrop.
 */
export function clampCropToImage(crop, imgWidth, imgHeight) {
  let { x, y, w, h } = crop;

  const scale = Math.min(1, imgWidth / w, imgHeight / h);
  if (scale < 1) {
    const cx = x + w / 2;
    const cy = y + h / 2;
    w *= scale;
    h *= scale;
    x = cx - w / 2;
    y = cy - h / 2;
  }

  x = Math.max(0, Math.min(x, imgWidth - w));
  y = Math.max(0, Math.min(y, imgHeight - h));

  return { x, y, w, h };
}

// Rounds a crop's fields to whole pixels - only ever applied at the true
// final output (see clampCropToImage's own doc comment for why intermediate
// containment checks must not round).
function roundCrop(crop) {
  return {
    x: Math.round(crop.x),
    y: Math.round(crop.y),
    w: Math.round(crop.w),
    h: Math.round(crop.h),
  };
}

/** Center-crop matching target's aspect ratio - used when no faces are found. */
export function fallbackCrop(imgWidth, imgHeight, target) {
  const { w, h } = maxBoxForAspect(imgWidth, imgHeight, target.aspectRatio);
  const x = (imgWidth - w) / 2;
  const y = (imgHeight - h) / 2;
  return roundCrop(clampCropToImage({ x, y, w, h }, imgWidth, imgHeight));
}

/**
 * Computes a recommended crop rectangle (source-image pixel space) for
 * `target`'s aspect ratio, prioritizing large faces staying fully visible.
 *
 * Strategy ("largest-face-priority"):
 *  1. Start from the largest face (plus a safety margin).
 *  2. Walk the remaining faces, largest to smallest, growing the working
 *     bounding box to include each one - but only accept a face if the
 *     resulting crop (grown to target's aspect ratio - using as much of the
 *     source image as fits, not just the minimum needed - then clamped to
 *     image bounds) still fully contains everything accepted so far. A face
 *     that doesn't fit without pushing an already-included, larger face out
 *     of frame is skipped (left to be cropped), never the other way around.
 *  3. Grow the final box the same way and clamp to the image.
 *
 * @param {number} imgWidth
 * @param {number} imgHeight
 * @param {Array<{x:number,y:number,w:number,h:number,score:number}>} faces
 * @param {{aspectRatio: number}} target
 * @param {{marginPercent?: number}} [options] - marginPercent (default 0.12)
 *   is the safety margin added around each face, as a fraction of its own
 *   width/height, before it's merged into the working crop.
 * @returns {{x:number,y:number,w:number,h:number}}
 */
export function computeRecommendedCrop(imgWidth, imgHeight, faces, target, options = {}) {
  const marginPercent = options.marginPercent ?? 0.12;

  if (!faces || faces.length === 0) {
    return fallbackCrop(imgWidth, imgHeight, target);
  }

  const sorted = scoreFaces(faces);
  let bbox = expandWithMargin(sorted[0], marginPercent);

  for (let i = 1; i < sorted.length; i++) {
    const faceBox = expandWithMargin(sorted[i], marginPercent);
    const candidate = union(bbox, faceBox);
    const candidateCrop = clampCropToImage(
      growToFillImage(candidate, imgWidth, imgHeight, target.aspectRatio),
      imgWidth,
      imgHeight,
    );
    // Require the FULL pre-clamp candidate (previously accepted faces plus
    // this one) to survive clamping - not just the previously accepted
    // bbox - so a face that only fits by clipping itself is also rejected.
    if (contains(candidateCrop, candidate)) {
      bbox = candidate;
    }
    // else: this face doesn't fit without displacing something already
    // included - skip it, per "larger faces take priority" (spec).
  }

  const grown = growToFillImage(bbox, imgWidth, imgHeight, target.aspectRatio);
  return roundCrop(clampCropToImage(grown, imgWidth, imgHeight));
}
