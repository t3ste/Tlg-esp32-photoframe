import fs from "fs";
import os from "os";
import path from "path";
import {
  buildMetadata,
  writeMetadataFile,
  readMetadataFile,
  metadataPathFor,
  FACECROP_SCHEMA_VERSION,
} from "../face-crop/metadata.js";

describe("metadataPathFor - filename association", () => {
  test("derives <basename>.facecrop.json next to the output image", () => {
    expect(metadataPathFor("/out/urlaub01.epdgz")).toBe(
      path.join("/out", "urlaub01.facecrop.json"),
    );
  });

  test("strips a suffix-decorated extension correctly", () => {
    expect(metadataPathFor("/out/photo_v2.png")).toBe(
      path.join("/out", "photo_v2.facecrop.json"),
    );
  });
});

describe("buildMetadata + write/read round-trip", () => {
  const target = {
    board: "waveshare_photopainter_73",
    width: 800,
    height: 480,
    aspectRatio: 800 / 480,
    orientation: "landscape",
  };
  const faces = [{ x: 10.4, y: 20.6, w: 100.2, h: 100.8, score: 0.9123 }];
  const recommendedCrop = { x: 5, y: 5, w: 400, h: 240 };
  const strategy = { mode: "largest-face-priority", marginPercent: 0.12, engine: "blazeface" };

  test("buildMetadata produces the documented schema shape", () => {
    const metadata = buildMetadata({
      sourcePath: "urlaub01.jpg",
      image: { width: 4032, height: 3024 },
      target,
      faces,
      recommendedCrop,
      strategy,
      generatorVersion: "esp32-photoframe-cli@1.0.0",
    });

    expect(metadata.schema).toBe(FACECROP_SCHEMA_VERSION);
    expect(metadata.source).toBe("urlaub01.jpg");
    expect(metadata.image).toEqual({ width: 4032, height: 3024 });
    expect(metadata.target.board).toBe("waveshare_photopainter_73");
    expect(metadata.target.orientation).toBe("landscape");
    expect(metadata.faces).toHaveLength(1);
    expect(metadata.faces[0].x).toBe(10); // rounded
    expect(metadata.recommended_crop).toEqual({ x: 5, y: 5, w: 400, h: 240 });
    expect(metadata.strategy.mode).toBe("largest-face-priority");
    expect(typeof metadata.timestamp).toBe("string");
    expect(metadata.generator_version).toBe("esp32-photoframe-cli@1.0.0");
  });

  test("writeMetadataFile + readMetadataFile round-trips exactly", () => {
    const metadata = buildMetadata({
      sourcePath: "urlaub01.jpg",
      image: { width: 4032, height: 3024 },
      target,
      faces,
      recommendedCrop,
      strategy,
      generatorVersion: "esp32-photoframe-cli@1.0.0",
    });

    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "facecrop-test-"));
    const metadataPath = path.join(tmpDir, "urlaub01.facecrop.json");
    try {
      writeMetadataFile(metadataPath, metadata);
      const reloaded = readMetadataFile(metadataPath);
      expect(reloaded).toEqual(metadata);
    } finally {
      fs.rmSync(tmpDir, { recursive: true, force: true });
    }
  });

  test("readMetadataFile rejects an unsupported schema version", () => {
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "facecrop-test-"));
    const metadataPath = path.join(tmpDir, "bad.facecrop.json");
    try {
      fs.writeFileSync(metadataPath, JSON.stringify({ schema: 999 }));
      expect(() => readMetadataFile(metadataPath)).toThrow(/Unsupported facecrop schema/);
    } finally {
      fs.rmSync(tmpDir, { recursive: true, force: true });
    }
  });
});
