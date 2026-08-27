import {
  computeRecommendedCrop,
  fallbackCrop,
  clampCropToImage,
  scoreFaces,
} from "../face-crop/crop-engine.js";

function boxContains(outer, inner, eps = 1) {
  return (
    inner.x >= outer.x - eps &&
    inner.y >= outer.y - eps &&
    inner.x + inner.w <= outer.x + outer.w + eps &&
    inner.y + inner.h <= outer.y + outer.h + eps
  );
}

const LANDSCAPE_TARGET = { aspectRatio: 800 / 480 };
const PORTRAIT_TARGET = { aspectRatio: 480 / 800 };

describe("scoreFaces", () => {
  test("sorts largest area first and annotates area", () => {
    const faces = [
      { x: 0, y: 0, w: 10, h: 10, score: 0.9 },
      { x: 0, y: 0, w: 100, h: 100, score: 0.5 },
    ];
    const sorted = scoreFaces(faces);
    expect(sorted[0].area).toBe(10000);
    expect(sorted[1].area).toBe(100);
  });
});

describe("fallbackCrop (no faces found)", () => {
  test("center-crops to the target aspect ratio when the image is wider than target", () => {
    const crop = fallbackCrop(2000, 1000, LANDSCAPE_TARGET);
    expect(crop.w / crop.h).toBeCloseTo(LANDSCAPE_TARGET.aspectRatio, 1);
    expect(crop.x).toBeGreaterThanOrEqual(0);
    expect(crop.y).toBeGreaterThanOrEqual(0);
    expect(crop.x + crop.w).toBeLessThanOrEqual(2000);
    expect(crop.y + crop.h).toBeLessThanOrEqual(1000);
  });

  test("center-crops when the image is taller than target", () => {
    const crop = fallbackCrop(1000, 2000, LANDSCAPE_TARGET);
    expect(crop.w / crop.h).toBeCloseTo(LANDSCAPE_TARGET.aspectRatio, 1);
    expect(crop.h).toBeLessThanOrEqual(2000);
  });

  test("computeRecommendedCrop falls back to center-crop when there are no faces", () => {
    const withFaces = computeRecommendedCrop(2000, 1000, [], LANDSCAPE_TARGET);
    const fallback = fallbackCrop(2000, 1000, LANDSCAPE_TARGET);
    expect(withFaces).toEqual(fallback);
  });
});

describe("computeRecommendedCrop - single large face", () => {
  test("recommended crop fully contains the face and matches target aspect ratio", () => {
    const face = { x: 400, y: 400, w: 200, h: 200, score: 0.95 };
    const crop = computeRecommendedCrop(1000, 1000, [face], LANDSCAPE_TARGET);

    expect(boxContains(crop, face)).toBe(true);
    expect(crop.w / crop.h).toBeCloseTo(LANDSCAPE_TARGET.aspectRatio, 1);
    expect(crop.x).toBeGreaterThanOrEqual(0);
    expect(crop.y).toBeGreaterThanOrEqual(0);
    expect(crop.x + crop.w).toBeLessThanOrEqual(1000);
    expect(crop.y + crop.h).toBeLessThanOrEqual(1000);
  });

  test("keeps the margin-expanded safety zone around the face inside the recommended crop", () => {
    // Comparing raw crop *sizes* across margin values isn't a reliable
    // signal any more now that computeRecommendedCrop prefers using as much
    // of the source image as it can (growToFillImage): once a crop is
    // already using the full available width/height, a bigger margin can't
    // push it any wider - it's already at the image's own ceiling either
    // way. What must still hold regardless is that the crop actually
    // contains the *margin-padded* face region, not just the bare face box -
    // that's what the margin is for.
    const face = { x: 400, y: 400, w: 200, h: 200, score: 0.95 };
    const marginPercent = 0.5;
    const crop = computeRecommendedCrop(1000, 1000, [face], LANDSCAPE_TARGET, { marginPercent });

    const mx = face.w * marginPercent;
    const my = face.h * marginPercent;
    const paddedFace = {
      x: face.x - mx,
      y: face.y - my,
      w: face.w + 2 * mx,
      h: face.h + 2 * my,
    };
    expect(boxContains(crop, paddedFace)).toBe(true);
  });
});

describe("computeRecommendedCrop - multiple faces, different sizes", () => {
  test("includes both faces when they comfortably fit together", () => {
    const large = { x: 300, y: 200, w: 150, h: 150, score: 0.9 };
    const small = { x: 550, y: 220, w: 40, h: 40, score: 0.8 };
    const crop = computeRecommendedCrop(1600, 900, [large, small], LANDSCAPE_TARGET);
    expect(boxContains(crop, large)).toBe(true);
    expect(boxContains(crop, small)).toBe(true);
  });

  test("prioritizes the largest face when a distant small face would push it out of frame", () => {
    const large = { x: 350, y: 250, w: 100, h: 100, score: 0.9 };
    const farTiny = { x: 3900, y: 1900, w: 10, h: 10, score: 0.3 };
    const crop = computeRecommendedCrop(4000, 2000, [large, farTiny], LANDSCAPE_TARGET);
    // The large face must always remain fully visible...
    expect(boxContains(crop, large)).toBe(true);
    // ...even though that means the tiny distant face is left out.
    expect(boxContains(crop, farTiny)).toBe(false);
  });
});

describe("computeRecommendedCrop - retains as much of the source image as possible", () => {
  test("crops only the dimension that actually needs it, not both, when the image has spare room", () => {
    // Real-world regression case: a 2592x1944 (4:3) photo of 3 people
    // spread across nearly its full width, targeting 5:3 (800x480). Only
    // trimming height is needed to reach 5:3 - 2592/(5/3) = 1555.2, i.e.
    // ~389px off the height, none off the width. An earlier version of
    // this algorithm cropped ~600px off the sides too even though nothing
    // required it, needlessly discarding image content (and, in an even
    // earlier version, one of the three faces along with it).
    const faces = [
      { x: 1548, y: 844, w: 515, h: 386, score: 0.9992 },
      { x: 213, y: 647, w: 579, h: 434, score: 0.9785 },
      { x: 834, y: 837, w: 385, h: 289, score: 0.7919 },
    ];
    const crop = computeRecommendedCrop(2592, 1944, faces, LANDSCAPE_TARGET, {
      marginPercent: 0.12,
    });

    expect(crop.x).toBe(0);
    expect(crop.w).toBe(2592);
    expect(crop.h).toBeLessThan(1944);
    for (const face of faces) {
      expect(boxContains(crop, face)).toBe(true);
    }
  });
});

describe("computeRecommendedCrop - margin is applied once to the accepted set, not per-face", () => {
  test("doesn't drop a face just because per-face margins would overflow, when the bare faces fit fine", () => {
    // Real-world regression case: a 972x1296 (3:4) photo with 3 detected
    // faces, targeting 5:3 (800x480), so the crop is always full image
    // width (972) and only ~583px tall. The two largest faces' *bare*
    // bounding boxes only span ~511px vertically - comfortably under 583 -
    // but margin-expanding each face individually *before* unioning them
    // pushed the union to ~585px, just barely over. That wrongly dropped
    // the second-largest face entirely, centering the crop on the largest
    // face alone and clipping the second face's top out of frame. Margin
    // must only ever affect the final crop, never which faces get in.
    const faces = [
      { x: 307, y: 541, w: 226, h: 301, score: 0.9999 },
      { x: 662, y: 736, w: 237, h: 316, score: 0.9932 },
      { x: -28, y: 613, w: 185, h: 246, score: 0.8465 },
    ];
    const crop = computeRecommendedCrop(972, 1296, faces, LANDSCAPE_TARGET, {
      marginPercent: 0.12,
    });

    expect(boxContains(crop, faces[0])).toBe(true);
    expect(boxContains(crop, faces[1])).toBe(true);
  });
});

describe("computeRecommendedCrop - clamping at image edges", () => {
  test("a face near the corner still yields an in-bounds, correctly-shaped crop", () => {
    const face = { x: 5, y: 5, w: 60, h: 60, score: 0.9 };
    const crop = computeRecommendedCrop(400, 300, [face], LANDSCAPE_TARGET);

    expect(crop.x).toBeGreaterThanOrEqual(0);
    expect(crop.y).toBeGreaterThanOrEqual(0);
    expect(crop.x + crop.w).toBeLessThanOrEqual(400);
    expect(crop.y + crop.h).toBeLessThanOrEqual(300);
    expect(crop.w / crop.h).toBeCloseTo(LANDSCAPE_TARGET.aspectRatio, 1);
  });
});

describe("clampCropToImage", () => {
  test("translates an in-bounds-sized crop back into the image without resizing it", () => {
    const crop = clampCropToImage({ x: -50, y: 10, w: 200, h: 100 }, 400, 300);
    expect(crop.w).toBe(200);
    expect(crop.h).toBe(100);
    expect(crop.x).toBeGreaterThanOrEqual(0);
  });

  test("uniformly scales down a crop bigger than the image, preserving aspect ratio", () => {
    const crop = clampCropToImage({ x: 0, y: 0, w: 800, h: 400 }, 400, 300);
    expect(crop.w).toBeLessThanOrEqual(400);
    expect(crop.h).toBeLessThanOrEqual(300);
    expect(crop.w / crop.h).toBeCloseTo(2, 1);
  });
});

describe("landscape vs portrait produce different crops for the same source", () => {
  test("same image and faces, different target orientation -> different recommended crop", () => {
    const faces = [
      { x: 300, y: 500, w: 150, h: 150, score: 0.9 },
      { x: 900, y: 300, w: 100, h: 100, score: 0.8 },
    ];
    const landscapeCrop = computeRecommendedCrop(1600, 1200, faces, LANDSCAPE_TARGET);
    const portraitCrop = computeRecommendedCrop(1600, 1200, faces, PORTRAIT_TARGET);

    expect(landscapeCrop).not.toEqual(portraitCrop);
    expect(landscapeCrop.w / landscapeCrop.h).toBeCloseTo(LANDSCAPE_TARGET.aspectRatio, 1);
    expect(portraitCrop.w / portraitCrop.h).toBeCloseTo(PORTRAIT_TARGET.aspectRatio, 1);
  });
});
