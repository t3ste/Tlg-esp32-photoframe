import { describe, it, expect } from "vitest";
import { parseDeviceWallClock, formatDeviceWallClock, TIMEZONE_PRESETS } from "./timezone.js";

describe("parseDeviceWallClock", () => {
  it("parses the device's space-separated wall-clock format", () => {
    const d = parseDeviceWallClock("2026-03-05 14:30:07");
    expect(d.getFullYear()).toBe(2026);
    expect(d.getMonth()).toBe(2); // 0-indexed
    expect(d.getDate()).toBe(5);
    expect(d.getHours()).toBe(14);
    expect(d.getMinutes()).toBe(30);
    expect(d.getSeconds()).toBe(7);
  });

  it("also accepts a 'T' separator", () => {
    const d = parseDeviceWallClock("2026-03-05T14:30:07");
    expect(d).not.toBeNull();
    expect(d.getHours()).toBe(14);
  });

  it("returns null for empty/malformed input", () => {
    expect(parseDeviceWallClock("")).toBeNull();
    expect(parseDeviceWallClock(null)).toBeNull();
    expect(parseDeviceWallClock("not a date")).toBeNull();
    // A DST-aware POSIX TZ string is not a wall-clock string - must not be
    // mistaken for one.
    expect(parseDeviceWallClock("CET-1CEST,M3.5.0/2,M10.5.0/3")).toBeNull();
  });
});

describe("formatDeviceWallClock", () => {
  it("round-trips through parseDeviceWallClock", () => {
    const original = "2026-01-09 09:05:00";
    expect(formatDeviceWallClock(parseDeviceWallClock(original))).toBe(original);
  });

  it("zero-pads single-digit fields", () => {
    const d = new Date(2026, 0, 9, 9, 5, 0);
    expect(formatDeviceWallClock(d)).toBe("2026-01-09 09:05:00");
  });
});

describe("TIMEZONE_PRESETS", () => {
  it("every preset has a title and a value", () => {
    for (const preset of TIMEZONE_PRESETS) {
      expect(typeof preset.title).toBe("string");
      expect(typeof preset.value).toBe("string");
      expect(preset.value.length).toBeGreaterThan(0);
    }
  });

  it("includes a DST-aware entry (the case the old numeric-offset field couldn't represent)", () => {
    expect(TIMEZONE_PRESETS.some((p) => p.value.includes(","))).toBe(true);
  });
});
