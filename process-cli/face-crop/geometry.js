/**
 * Target display geometry resolution for face-aware cropping.
 *
 * Combines --board / --resolution / --display-size-mm / --orientation into a
 * single normalized target geometry that the crop engine can work with,
 * following a fixed precedence: explicit --orientation always wins; among
 * width/height sources, --resolution beats --board beats the CLI's existing
 * --display-width/--display-height defaults; among orientation *hints* (when
 * --orientation isn't explicitly given), --resolution beats --display-size-mm
 * beats the board's own default orientation.
 */

import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
import embeddedBoards from "./boards.json" with { type: "json" };

const __dirname = path.dirname(fileURLToPath(import.meta.url));

// The firmware's own board list is the source of truth for width/height when
// running from a full source checkout (repo root two levels up from here).
// process-cli is also published standalone to npm (see README's "Publishing
// to npm" section), where that file won't exist - fall back to the embedded
// copy below in that case, so board lookups never hard-fail.
const REPO_BOARDS_JSON = path.join(__dirname, "..", "..", "boards", "boards.json");

function loadRepoBoards() {
  try {
    const raw = fs.readFileSync(REPO_BOARDS_JSON, "utf8");
    const list = JSON.parse(raw);
    const map = {};
    for (const entry of list) {
      if (!entry.id || !Array.isArray(entry.resolution)) continue;
      const [width, height] = entry.resolution;
      map[entry.id] = {
        width,
        height,
        orientation: width >= height ? "landscape" : "portrait",
        // displaySizeMm isn't tracked in the firmware's boards.json - reuse
        // the embedded approximation (derived from panel diagonal) if we
        // have one for this id, otherwise omit it entirely.
        displaySizeMm: embeddedBoards[entry.id]?.displaySizeMm,
      };
    }
    return map;
  } catch {
    return null;
  }
}

let boardProfilesCache = null;

/** Returns the id -> {width, height, orientation, displaySizeMm} board map. */
export function getBoardProfiles() {
  if (boardProfilesCache) return boardProfilesCache;
  boardProfilesCache = loadRepoBoards() || embeddedBoards;
  return boardProfilesCache;
}

/** Returns a single board's profile, or null if the id is unknown. */
export function getBoardProfile(boardId) {
  const profiles = getBoardProfiles();
  return profiles[boardId] || null;
}

/**
 * Parses a "WIDTHxHEIGHT" string (case-insensitive 'x' separator, integers
 * or decimals). Returns null if the string doesn't match.
 */
export function parseWidthHeight(str) {
  if (!str) return null;
  const m = /^\s*(\d+(?:\.\d+)?)\s*x\s*(\d+(?:\.\d+)?)\s*$/i.exec(str);
  if (!m) return null;
  return { width: parseFloat(m[1]), height: parseFloat(m[2]) };
}

export function orientationFromDims(w, h) {
  if (w > h) return "landscape";
  if (h > w) return "portrait";
  return "square";
}

/**
 * Normalizes --board / --resolution / --display-size-mm / --orientation into
 * a single target geometry: { board, width, height, aspectRatio, orientation,
 * warnings }.
 *
 * @param {Object} opts
 * @param {string} [opts.board] - Board id (see boards/boards.json / the
 *   embedded fallback in face-crop/boards.json).
 * @param {string} [opts.resolution] - "WIDTHxHEIGHT" in pixels.
 * @param {string} [opts.displaySizeMm] - "WIDTHxHEIGHT" in mm, used only to
 *   help derive orientation when --resolution/--board don't already fix it.
 * @param {string} [opts.orientation] - "landscape" | "portrait" | "auto".
 *   Anything other than "auto" is treated as an explicit override, taking
 *   priority over every other source.
 * @param {number} [opts.fallbackWidth] - Used when neither --board nor
 *   --resolution is given (mirrors the CLI's existing --display-width).
 * @param {number} [opts.fallbackHeight] - Same, for --display-height.
 * @returns {{board: (string|undefined), width: number, height: number,
 *   aspectRatio: number, orientation: string, warnings: string[]}}
 */
export function normalizeTargetGeometry(opts = {}) {
  const {
    board,
    resolution,
    displaySizeMm,
    orientation = "auto",
    fallbackWidth = 800,
    fallbackHeight = 480,
  } = opts;

  const warnings = [];
  let boardProfile = null;
  if (board) {
    boardProfile = getBoardProfile(board);
    if (!boardProfile) {
      const known = Object.keys(getBoardProfiles()).join(", ");
      throw new Error(`Unknown --board "${board}". Known boards: ${known}`);
    }
  }

  const resolutionDims = resolution ? parseWidthHeight(resolution) : null;
  if (resolution && !resolutionDims) {
    throw new Error(
      `Invalid --resolution "${resolution}", expected WIDTHxHEIGHT (e.g. 800x480)`,
    );
  }

  const mmDims = displaySizeMm ? parseWidthHeight(displaySizeMm) : null;
  if (displaySizeMm && !mmDims) {
    throw new Error(
      `Invalid --display-size-mm "${displaySizeMm}", expected WIDTHxHEIGHT (e.g. 160x96)`,
    );
  }

  // --- Pixel magnitudes (the two side lengths, order not yet meaningful) ---
  // Precedence: --resolution > --board > CLI display-width/height defaults.
  let dimA, dimB;
  if (resolutionDims) {
    dimA = resolutionDims.width;
    dimB = resolutionDims.height;
    if (board && boardProfile) {
      warnings.push(
        `Board "${board}"'s default resolution (${boardProfile.width}x${boardProfile.height}) ` +
          `was overridden by explicit --resolution ${resolution}`,
      );
    }
  } else if (boardProfile) {
    dimA = boardProfile.width;
    dimB = boardProfile.height;
  } else {
    dimA = fallbackWidth;
    dimB = fallbackHeight;
  }

  // --- Orientation ---
  // Precedence: explicit --orientation > --resolution-derived >
  // --display-size-mm-derived > board default > "landscape".
  const explicitOrientation = orientation !== "auto" ? orientation : null;
  const candidates = [];
  if (explicitOrientation) {
    candidates.push({ source: "--orientation", value: explicitOrientation });
  }
  if (resolutionDims) {
    candidates.push({
      source: "--resolution",
      value: orientationFromDims(resolutionDims.width, resolutionDims.height),
    });
  }
  if (mmDims) {
    candidates.push({
      source: "--display-size-mm",
      value: orientationFromDims(mmDims.width, mmDims.height),
    });
  }
  if (boardProfile) {
    candidates.push({ source: `--board ${board}`, value: boardProfile.orientation });
  }

  let finalOrientation;
  if (candidates.length > 0) {
    finalOrientation = candidates[0].value;
    const distinct = new Set(candidates.map((c) => c.value));
    if (distinct.size > 1) {
      const detail = candidates.map((c) => `${c.source}=${c.value}`).join(", ");
      warnings.push(
        `Conflicting orientation hints (${detail}) - using "${finalOrientation}" ` +
          `from ${candidates[0].source}`,
      );
    }
  } else {
    finalOrientation = "landscape";
  }

  // --- Reconcile magnitudes with final orientation ---
  // Same aspect ratio can be landscape or portrait; reorder dimA/dimB (never
  // discard either value) so the result actually matches finalOrientation.
  const major = Math.max(dimA, dimB);
  const minor = Math.min(dimA, dimB);
  let width, height;
  if (finalOrientation === "portrait") {
    width = minor;
    height = major;
  } else if (finalOrientation === "landscape") {
    width = major;
    height = minor;
  } else {
    // "square": dimA/dimB should already be equal in practice; if not
    // (only possible via a manually contrived config), keep source order.
    width = dimA;
    height = dimB;
  }
  return {
    board,
    width,
    height,
    aspectRatio: width / height,
    orientation: width === height ? "square" : finalOrientation,
    warnings,
  };
}
