/**
 * Jest test suite for image and thumbnail orientation in --serve mode
 *
 * Tests that for all 3 formats (JPG, PNG, BMP):
 * - Portrait source images produce portrait thumbnails (240x400)
 * - Portrait source images produce landscape served images (800x480)
 * - Landscape source images produce landscape thumbnails (400x240)
 * - Landscape source images produce landscape served images (800x480)
 */

import { loadImage } from "canvas";
import fetch from "node-fetch";
import path from "path";
import { fileURLToPath } from "url";
import { createImageServer } from "../server.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const TEST_IMAGES = {
  portrait: "portrait.jpg",
  landscape: "landscape.jpg",
};

// Port mapping for each format to avoid conflicts
const PORT_MAP = {
  jpg: 9000,
  png: 9001,
  bmp: 9002,
};

async function getImageDimensions(buffer) {
  const img = await loadImage(buffer);
  return { width: img.width, height: img.height };
}

async function fetchImage(port) {
  const response = await fetch(`http://localhost:${port}/image`);
  if (!response.ok) {
    throw new Error(
      `Failed to fetch image: ${response.status} ${response.statusText}`,
    );
  }

  const contentType = response.headers.get("content-type");
  const thumbnailUrl = response.headers.get("x-thumbnail-url");
  const buffer = await response.arrayBuffer();

  return {
    buffer: Buffer.from(buffer),
    contentType,
    thumbnailUrl,
  };
}

async function fetchThumbnail(thumbnailUrl) {
  const response = await fetch(thumbnailUrl);
  if (!response.ok) {
    throw new Error(
      `Failed to fetch thumbnail: ${response.status} ${response.statusText}`,
    );
  }

  const buffer = await response.arrayBuffer();
  return Buffer.from(buffer);
}

describe.each(["jpg", "png", "bmp"])(
  "Image orientation tests - %s format",
  (format) => {
    let server;
    const port = PORT_MAP[format];

    beforeAll(async () => {
      // Start server for this format
      const albumDir = path.join(__dirname, "test-albums");
      server = await createImageServer(
        albumDir,
        port,
        format,
        null, // devicePalette
        null, // deviceSettings
        { silent: true }, // Don't log to console during tests
      );
    }, 30000); // 30 second timeout for server startup

    afterAll(async () => {
      // Stop server
      if (server) {
        await new Promise((resolve) => {
          server.close(resolve);
        });
      }
    });

    describe("Portrait source image", () => {
      let imageDims;
      let thumbDims;

      beforeAll(async () => {
        const thumbUrl = `http://localhost:${port}/thumbnail?file=${TEST_IMAGES.portrait}`;

        // Fetch main image
        const { buffer, contentType } = await fetchImage(port);

        // Verify content type
        const expectedType =
          format === "jpg" ? "image/jpeg" : `image/${format}`;
        expect(contentType).toBe(expectedType);

        imageDims = await getImageDimensions(buffer);

        // Fetch thumbnail
        const thumbBuffer = await fetchThumbnail(thumbUrl);
        thumbDims = await getImageDimensions(thumbBuffer);
      });

      test("served image should be landscape (800x480)", () => {
        expect(imageDims.width).toBe(800);
        expect(imageDims.height).toBe(480);
      });

      test("thumbnail should be portrait (240x400)", () => {
        expect(thumbDims.width).toBe(240);
        expect(thumbDims.height).toBe(400);
      });
    });

    describe("Landscape source image", () => {
      let imageDims;
      let thumbDims;

      beforeAll(async () => {
        const thumbUrl = `http://localhost:${port}/thumbnail?file=${TEST_IMAGES.landscape}`;

        // Fetch main image
        const { buffer, contentType } = await fetchImage(port);

        // Verify content type
        const expectedType =
          format === "jpg" ? "image/jpeg" : `image/${format}`;
        expect(contentType).toBe(expectedType);

        imageDims = await getImageDimensions(buffer);

        // Fetch thumbnail
        const thumbBuffer = await fetchThumbnail(thumbUrl);
        thumbDims = await getImageDimensions(thumbBuffer);
      });

      test("served image should be landscape (800x480)", () => {
        expect(imageDims.width).toBe(800);
        expect(imageDims.height).toBe(480);
      });

      test("thumbnail should be landscape (400x240)", () => {
        expect(thumbDims.width).toBe(400);
        expect(thumbDims.height).toBe(240);
      });
    });
  },
);
