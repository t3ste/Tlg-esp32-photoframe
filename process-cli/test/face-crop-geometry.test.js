import {
  parseWidthHeight,
  normalizeTargetGeometry,
  getBoardProfile,
  getBoardProfiles,
  orientationFromDims,
} from "../face-crop/geometry.js";

describe("orientationFromDims", () => {
  test("wider than tall is landscape", () => {
    expect(orientationFromDims(2592, 1944)).toBe("landscape");
  });

  test("taller than wide is portrait", () => {
    expect(orientationFromDims(1944, 2592)).toBe("portrait");
  });

  test("equal width and height is square", () => {
    expect(orientationFromDims(2880, 2880)).toBe("square");
  });
});

describe("parseWidthHeight", () => {
  test("parses a valid WxH string", () => {
    expect(parseWidthHeight("800x480")).toEqual({ width: 800, height: 480 });
  });

  test("is case-insensitive on the separator", () => {
    expect(parseWidthHeight("800X480")).toEqual({ width: 800, height: 480 });
  });

  test("accepts decimals (for mm sizes)", () => {
    expect(parseWidthHeight("160.5x96.2")).toEqual({
      width: 160.5,
      height: 96.2,
    });
  });

  test("returns null for garbage input", () => {
    expect(parseWidthHeight("not-a-dimension")).toBeNull();
    expect(parseWidthHeight("")).toBeNull();
    expect(parseWidthHeight(undefined)).toBeNull();
  });
});

describe("board mapping", () => {
  test("known board resolves to a profile with width/height/orientation", () => {
    const profile = getBoardProfile("waveshare_photopainter_73");
    expect(profile).not.toBeNull();
    expect(profile.width).toBe(800);
    expect(profile.height).toBe(480);
    expect(profile.orientation).toBe("landscape");
  });

  test("a portrait-shaped board is reported as portrait", () => {
    const profile = getBoardProfile("seeedstudio_xiao_ee02");
    expect(profile).not.toBeNull();
    expect(profile.orientation).toBe("portrait");
  });

  test("unknown board returns null", () => {
    expect(getBoardProfile("not_a_real_board")).toBeNull();
  });

  test("board list is non-empty", () => {
    expect(Object.keys(getBoardProfiles()).length).toBeGreaterThan(0);
  });
});

describe("normalizeTargetGeometry - resolution parsing", () => {
  test("--resolution sets width/height/aspectRatio directly", () => {
    const target = normalizeTargetGeometry({
      resolution: "800x480",
      orientation: "auto",
    });
    expect(target.width).toBe(800);
    expect(target.height).toBe(480);
    expect(target.aspectRatio).toBeCloseTo(800 / 480);
  });

  test("invalid --resolution throws", () => {
    expect(() => normalizeTargetGeometry({ resolution: "bogus" })).toThrow();
  });
});

describe("normalizeTargetGeometry - display-size-mm parsing", () => {
  test("invalid --display-size-mm throws", () => {
    expect(() => normalizeTargetGeometry({ displaySizeMm: "bogus" })).toThrow();
  });

  test("mm alone (no --resolution/--board) still falls back to fallbackWidth/Height for pixel size", () => {
    const target = normalizeTargetGeometry({
      displaySizeMm: "160x96",
      fallbackWidth: 800,
      fallbackHeight: 480,
    });
    // mm only informs orientation, not pixel counts
    expect(target.width).toBe(800);
    expect(target.height).toBe(480);
  });
});

describe("normalizeTargetGeometry - auto orientation derivation", () => {
  test("landscape --resolution auto-derives landscape orientation", () => {
    const target = normalizeTargetGeometry({
      resolution: "800x480",
      orientation: "auto",
    });
    expect(target.orientation).toBe("landscape");
  });

  test("portrait --resolution auto-derives portrait orientation", () => {
    const target = normalizeTargetGeometry({
      resolution: "480x800",
      orientation: "auto",
    });
    expect(target.orientation).toBe("portrait");
    expect(target.width).toBe(480);
    expect(target.height).toBe(800);
  });

  test("--display-size-mm alone auto-derives orientation when no --resolution given", () => {
    const target = normalizeTargetGeometry({
      displaySizeMm: "96x160", // taller than wide
      fallbackWidth: 800,
      fallbackHeight: 480,
      orientation: "auto",
    });
    expect(target.orientation).toBe("portrait");
  });

  test("--board alone auto-derives orientation from the board's own shape", () => {
    const target = normalizeTargetGeometry({
      board: "seeedstudio_xiao_ee02",
      orientation: "auto",
    });
    expect(target.orientation).toBe("portrait");
  });

  test("square resolution is reported as square", () => {
    const target = normalizeTargetGeometry({
      resolution: "600x600",
      orientation: "auto",
    });
    expect(target.orientation).toBe("square");
    expect(target.aspectRatio).toBeCloseTo(1);
  });
});

describe("normalizeTargetGeometry - explicit orientation override", () => {
  test("explicit orientation overrides resolution-derived orientation and reorders dims", () => {
    // 800x480 numbers are landscape-shaped, but user explicitly wants portrait.
    const target = normalizeTargetGeometry({
      resolution: "800x480",
      orientation: "portrait",
    });
    expect(target.orientation).toBe("portrait");
    expect(target.height).toBeGreaterThanOrEqual(target.width);
    // Same magnitude pair as the input, just reordered.
    expect([target.width, target.height].sort((a, b) => a - b)).toEqual([
      480, 800,
    ]);
  });

  test("explicit orientation overrides board default", () => {
    const target = normalizeTargetGeometry({
      board: "waveshare_photopainter_73", // native landscape
      orientation: "portrait",
    });
    expect(target.orientation).toBe("portrait");
    expect(target.height).toBeGreaterThanOrEqual(target.width);
  });

  test("landscape vs portrait for the identical aspect ratio produce swapped width/height", () => {
    const landscape = normalizeTargetGeometry({
      resolution: "800x480",
      orientation: "landscape",
    });
    const portrait = normalizeTargetGeometry({
      resolution: "800x480",
      orientation: "portrait",
    });
    expect(landscape.width).toBe(portrait.height);
    expect(landscape.height).toBe(portrait.width);
    expect(landscape.aspectRatio).toBeCloseTo(1 / portrait.aspectRatio);
  });
});

describe("normalizeTargetGeometry - precedence and warnings", () => {
  test("--resolution overriding --board's own resolution produces a warning", () => {
    const target = normalizeTargetGeometry({
      board: "waveshare_photopainter_73", // 800x480
      resolution: "1200x1600", // different board's shape
      orientation: "auto",
    });
    expect(target.width).toBe(1200);
    expect(target.height).toBe(1600);
    expect(target.warnings.length).toBeGreaterThan(0);
  });

  test("unknown --board throws a clear error", () => {
    expect(() => normalizeTargetGeometry({ board: "no_such_board" })).toThrow(
      /Unknown --board/,
    );
  });
});
