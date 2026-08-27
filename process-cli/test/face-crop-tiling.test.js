import {
  computeTiles,
  cropImageData,
  mergeOverlappingDetections,
} from "../face-crop/blazeface-adapter.js";

describe("computeTiles", () => {
  test("a 1x1 grid covers the whole image in a single tile", () => {
    const tiles = computeTiles(1000, 800, 1);
    expect(tiles).toHaveLength(1);
    expect(tiles[0]).toEqual({ x: 0, y: 0, w: 1000, h: 800 });
  });

  test("a 2x2 grid produces 4 tiles that fully cover the image, each within bounds", () => {
    const tiles = computeTiles(1000, 800, 2, 0.2);
    expect(tiles).toHaveLength(4);
    for (const t of tiles) {
      expect(t.x).toBeGreaterThanOrEqual(0);
      expect(t.y).toBeGreaterThanOrEqual(0);
      expect(t.x + t.w).toBeLessThanOrEqual(1000);
      expect(t.y + t.h).toBeLessThanOrEqual(800);
    }
    // The last tile in each row/column must reach the far edge - otherwise a
    // face sitting right at the edge wouldn't be covered by any tile at all.
    const maxRight = Math.max(...tiles.map((t) => t.x + t.w));
    const maxBottom = Math.max(...tiles.map((t) => t.y + t.h));
    expect(maxRight).toBe(1000);
    expect(maxBottom).toBe(800);
  });

  test("adjacent tiles actually overlap, not just touch or gap", () => {
    // `overlap` inflates each tile beyond an even 1/gridSize split (so a face
    // near a boundary still lands fully inside at least one tile) - it isn't
    // itself the resulting adjacent-tile overlap fraction, so this only
    // checks that tiles meaningfully overlap, not an exact ratio.
    const tiles = computeTiles(1000, 800, 2, 0.2);
    const left = tiles[0];
    const right = tiles[1];
    const overlapWidth = left.x + left.w - right.x;
    expect(overlapWidth).toBeGreaterThan(left.w * 0.1);
  });

  test("a grid larger than the image still clamps tile size to the image itself", () => {
    const tiles = computeTiles(100, 80, 5, 0.2);
    for (const t of tiles) {
      expect(t.w).toBeLessThanOrEqual(100);
      expect(t.h).toBeLessThanOrEqual(80);
    }
  });
});

describe("cropImageData", () => {
  test("extracts the correct sub-rectangle of pixel data", () => {
    // 4x4 RGBA image where each pixel's red channel encodes its flat index,
    // so we can verify the crop pulled the right pixels from the right rows.
    const width = 4;
    const height = 4;
    const data = new Uint8ClampedArray(width * height * 4);
    for (let i = 0; i < width * height; i++) {
      data[i * 4] = i;
      data[i * 4 + 3] = 255;
    }
    const imageData = { data, width, height };

    const crop = cropImageData(imageData, 1, 1, 2, 2);
    expect(crop.width).toBe(2);
    expect(crop.height).toBe(2);
    // Source indices for a (1,1)-(2,2) crop of a 4-wide image: row1 -> [5,6], row2 -> [9,10]
    expect(crop.data[0]).toBe(5);
    expect(crop.data[4]).toBe(6);
    expect(crop.data[8]).toBe(9);
    expect(crop.data[12]).toBe(10);
  });
});

describe("mergeOverlappingDetections", () => {
  test("keeps distinct, non-overlapping faces as-is", () => {
    const faces = [
      { x: 0, y: 0, w: 50, h: 50, score: 0.9 },
      { x: 500, y: 500, w: 50, h: 50, score: 0.8 },
    ];
    expect(mergeOverlappingDetections(faces)).toHaveLength(2);
  });

  test("collapses the same face detected in both the full pass and a tile pass, keeping the higher score", () => {
    const wholeImagePass = { x: 100, y: 100, w: 60, h: 60, score: 0.7 };
    const tilePass = { x: 102, y: 98, w: 58, h: 62, score: 0.95 };
    const merged = mergeOverlappingDetections([wholeImagePass, tilePass], 0.3);
    expect(merged).toHaveLength(1);
    expect(merged[0].score).toBe(0.95);
  });

  test("does not merge two genuinely different, merely nearby faces", () => {
    const faceA = { x: 0, y: 0, w: 50, h: 50, score: 0.9 };
    const faceB = { x: 200, y: 0, w: 50, h: 50, score: 0.85 };
    const merged = mergeOverlappingDetections([faceA, faceB], 0.3);
    expect(merged).toHaveLength(2);
  });
});
