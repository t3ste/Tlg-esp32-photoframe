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

  test("applies a safety margin around the face rather than a tight fit", () => {
    const face = { x: 400, y: 400, w: 200, h: 200, score: 0.95 };
    const tight = computeRecommendedCrop(1000, 1000, [face], LANDSCAPE_TARGET, {
      marginPercent: 0,
    });
    const padded = computeRecommendedCrop(1000, 1000, [face], LANDSCAPE_TARGET, {
      marginPercent: 0.5,
    });
    expect(padded.w).toBeGreaterThan(tight.w);
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
