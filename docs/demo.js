// Tab switching function
function switchTab(tabName) {
  document
    .querySelectorAll(".nav-tab")
    .forEach((t) => t.classList.remove("active"));
  document
    .querySelectorAll(".tab-content")
    .forEach((c) => c.classList.remove("active"));

  const tab = document.querySelector(`.nav-tab[data-tab="${tabName}"]`);
  const content = document.getElementById(`${tabName}-tab`);

  if (tab && content) {
    tab.classList.add("active");
    content.classList.add("active");
    window.location.hash = tabName;
  }
}

// Tab click handlers
document.querySelectorAll(".nav-tab").forEach((tab) => {
  tab.addEventListener("click", () => {
    switchTab(tab.dataset.tab);
  });
});

// Handle hash navigation on page load and hash change
function handleHashChange() {
  const hash = window.location.hash.substring(1);
  if (hash && (hash === "demo" || hash === "flash")) {
    switchTab(hash);
  } else if (!hash) {
    switchTab("demo"); // Default to demo tab
  }
}

window.addEventListener("hashchange", handleHashChange);
handleHashChange(); // Initialize on page load

// Fetch the latest release info from GitHub API
async function getLatestRelease() {
  try {
    const response = await fetch(
      "https://api.github.com/repos/aitjcize/esp32-photoframe/releases/latest",
    );
    const data = await response.json();

    const versionInfo = document.getElementById("version-info");
    if (versionInfo) {
      versionInfo.textContent = `Latest Version: ${data.tag_name} (${new Date(data.published_at).toLocaleDateString()})`;
      versionInfo.classList.remove("loading");
    }
  } catch (error) {
    console.error("Error fetching release:", error);
    const versionInfo = document.getElementById("version-info");
    if (versionInfo) {
      versionInfo.textContent = "Version: 1.0.0";
      versionInfo.classList.remove("loading");
    }
  }
}

getLatestRelease();

// Demo functionality
import {
  processImage,
  getCanvasContext,
  getPreset,
} from "./image-processor.js";

let currentImageFile = null;
let sourceCanvas = null; // Store as canvas instead of ImageData
let sliderPosition = 0.3;
let isDragging = false;
let currentPreset = "cdr";

// Initialize with CDR preset
const defaultPreset = getPreset("cdr");
const enhancedParams = {
  ...defaultPreset,
  renderMeasured: true,
};

const stockParams = {
  processingMode: "stock",
  renderMeasured: true,
  ditherAlgorithm: "floyd-steinberg",
};

const fileInput = document.getElementById("fileInput");
const dropZone = document.getElementById("dropZone");
const uploadArea = document.getElementById("uploadArea");
const previewSection = document.getElementById("previewSection");

dropZone.addEventListener("click", () => fileInput.click());

fileInput.addEventListener("change", async (e) => {
  const file = e.target.files[0];
  if (!file) return;

  document.getElementById("fileName").textContent = `Selected: ${file.name}`;
  currentImageFile = file;
  uploadArea.style.display = "none";
  previewSection.style.display = "block";

  await loadImagePreview(file);
});

dropZone.addEventListener("dragover", (e) => {
  e.preventDefault();
  e.stopPropagation();
  dropZone.classList.add("drag-over");
});

dropZone.addEventListener("dragleave", (e) => {
  e.preventDefault();
  e.stopPropagation();
  dropZone.classList.remove("drag-over");
});

dropZone.addEventListener("drop", async (e) => {
  e.preventDefault();
  e.stopPropagation();
  dropZone.classList.remove("drag-over");

  const file = e.dataTransfer.files[0];
  if (file && file.type.match("image.*")) {
    document.getElementById("fileName").textContent = `Selected: ${file.name}`;
    currentImageFile = file;
    uploadArea.style.display = "none";
    previewSection.style.display = "block";

    await loadImagePreview(file);
  }
});

const sliderContainer = document.getElementById("sliderContainer");
const comparisonWrapper = document.getElementById("comparisonWrapper");
const enhancedCanvas = document.getElementById("enhancedCanvas");

function updateSliderPosition(clientX) {
  const rect = comparisonWrapper.getBoundingClientRect();
  const x = clientX - rect.left;
  sliderPosition = Math.max(0, Math.min(1, x / rect.width));

  sliderContainer.style.left = `${sliderPosition * 100}%`;
  // Clip from right: show stock on left side (clip right portion)
  const stockCanvas = document.getElementById("stockCanvas");
  stockCanvas.style.clipPath = `inset(0 ${(1 - sliderPosition) * 100}% 0 0)`;
}

sliderContainer.addEventListener("mousedown", (e) => {
  isDragging = true;
  e.preventDefault();
});

document.addEventListener("mousemove", (e) => {
  if (isDragging) {
    updateSliderPosition(e.clientX);
  }
});

document.addEventListener("mouseup", () => {
  isDragging = false;
});

sliderContainer.addEventListener("touchstart", (e) => {
  isDragging = true;
  e.preventDefault();
});

document.addEventListener("touchmove", (e) => {
  if (isDragging && e.touches.length > 0) {
    updateSliderPosition(e.touches[0].clientX);
  }
});

document.addEventListener("touchend", () => {
  isDragging = false;
});

function loadImage(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = reject;
      img.src = e.target.result;
    };
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
}

async function loadImagePreview(file) {
  const stockCanvas = document.getElementById("stockCanvas");
  const enhancedCanvas = document.getElementById("enhancedCanvas");

  try {
    const img = await loadImage(file);
    // Store as canvas (matches webapp behavior)
    sourceCanvas = document.createElement("canvas");
    sourceCanvas.width = img.width;
    sourceCanvas.height = img.height;
    const sourceCtx = getCanvasContext(sourceCanvas);
    sourceCtx.drawImage(img, 0, 0);

    updatePreviews();
  } catch (error) {
    console.error("Error loading preview:", error);
    alert("Error loading image preview");
  }
}

function drawCurveVisualization() {
  const canvas = document.getElementById("curveCanvas");
  const ctx = getCanvasContext(canvas);
  const width = canvas.width;
  const height = canvas.height;
  const padding = 40;

  ctx.clearRect(0, 0, width, height);

  ctx.strokeStyle = "#ccc";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(padding, height - padding);
  ctx.lineTo(width - padding, height - padding);
  ctx.lineTo(width - padding, padding);
  ctx.stroke();

  ctx.strokeStyle = "#e0e0e0";
  ctx.lineWidth = 1;
  ctx.setLineDash([5, 5]);
  ctx.beginPath();
  ctx.moveTo(padding, height - padding);
  ctx.lineTo(width - padding, padding);
  ctx.stroke();
  ctx.setLineDash([]);

  const { strength, shadowBoost, highlightCompress, midpoint } = enhancedParams;
  ctx.strokeStyle = "#667eea";
  ctx.lineWidth = 3;
  ctx.beginPath();

  const graphWidth = width - 2 * padding;
  const graphHeight = height - 2 * padding;

  for (let i = 0; i <= graphWidth; i++) {
    const normalized = i / graphWidth;
    let result;

    if (normalized <= midpoint) {
      const shadowVal = normalized / midpoint;
      result = Math.pow(shadowVal, 1.0 - strength * shadowBoost) * midpoint;
    } else {
      const highlightVal = (normalized - midpoint) / (1.0 - midpoint);
      result =
        midpoint +
        Math.pow(highlightVal, 1.0 + strength * highlightCompress) *
          (1.0 - midpoint);
    }

    const x = padding + i;
    const y = height - padding - result * graphHeight;

    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }

  ctx.stroke();

  ctx.strokeStyle = "#ff6b6b";
  ctx.lineWidth = 2;
  ctx.setLineDash([3, 3]);
  const midX = padding + midpoint * graphWidth;
  ctx.beginPath();
  ctx.moveTo(midX, height - padding);
  ctx.lineTo(midX, padding);
  ctx.stroke();
  ctx.setLineDash([]);
}

function updatePreviews() {
  if (!sourceCanvas) return;

  drawCurveVisualization();

  // Always pass landscape dimensions - processImage will handle orientation
  // This matches the webapp behavior exactly
  const { canvas: enhancedProcessed } = processImage(
    sourceCanvas,
    enhancedParams,
    null, // no custom palette
    {
      verbose: false,
      skipRotation: true, // Don't rotate in browser preview
      targetWidth: 800,
      targetHeight: 480,
    },
  );

  // Update enhanced canvas dimensions and draw
  const enhancedCanvas = document.getElementById("enhancedCanvas");
  enhancedCanvas.width = enhancedProcessed.width;
  enhancedCanvas.height = enhancedProcessed.height;
  const enhancedCtx = getCanvasContext(enhancedCanvas);
  enhancedCtx.drawImage(enhancedProcessed, 0, 0);

  // Process stock version (no tone mapping, theoretical palette)
  const { canvas: stockProcessed } = processImage(
    sourceCanvas,
    stockParams,
    null, // no custom palette
    {
      verbose: false,
      skipRotation: true, // Don't rotate in browser preview
      targetWidth: 800,
      targetHeight: 480,
    },
  );

  // Update stock canvas dimensions and draw
  const stockCanvas = document.getElementById("stockCanvas");
  stockCanvas.width = stockProcessed.width;
  stockCanvas.height = stockProcessed.height;
  const stockCtx = getCanvasContext(stockCanvas);
  stockCtx.drawImage(stockProcessed, 0, 0);

  // Set initial slider position to 30%
  sliderContainer.style.left = `${sliderPosition * 100}%`;
  stockCanvas.style.clipPath = `inset(0 ${(1 - sliderPosition) * 100}% 0 0)`;
}

document.getElementById("exposure").addEventListener("input", (e) => {
  enhancedParams.exposure = parseFloat(e.target.value);
  document.getElementById("exposureValue").textContent = parseFloat(
    e.target.value,
  ).toFixed(2);
  switchToCustomPreset();
  updatePreviews();
});

document.getElementById("saturation").addEventListener("input", (e) => {
  enhancedParams.saturation = parseFloat(e.target.value);
  document.getElementById("saturationValue").textContent = parseFloat(
    e.target.value,
  ).toFixed(2);
  switchToCustomPreset();
  updatePreviews();
});

document.getElementById("contrast").addEventListener("input", (e) => {
  enhancedParams.contrast = parseFloat(e.target.value);
  document.getElementById("contrastValue").textContent = parseFloat(
    e.target.value,
  ).toFixed(2);
  switchToCustomPreset();
  updatePreviews();
});

document.getElementById("scurveStrength").addEventListener("input", (e) => {
  enhancedParams.strength = parseFloat(e.target.value);
  document.getElementById("strengthValue").textContent = parseFloat(
    e.target.value,
  ).toFixed(2);
  switchToCustomPreset();
  updatePreviews();
});

document.getElementById("scurveShadow").addEventListener("input", (e) => {
  enhancedParams.shadowBoost = parseFloat(e.target.value);
  document.getElementById("shadowValue").textContent = parseFloat(
    e.target.value,
  ).toFixed(2);
  switchToCustomPreset();
  updatePreviews();
});

document.getElementById("scurveHighlight").addEventListener("input", (e) => {
  enhancedParams.highlightCompress = parseFloat(e.target.value);
  document.getElementById("highlightValue").textContent = parseFloat(
    e.target.value,
  ).toFixed(2);
  switchToCustomPreset();
  updatePreviews();
});

document.getElementById("scurveMidpoint").addEventListener("input", (e) => {
  enhancedParams.midpoint = parseFloat(e.target.value);
  document.getElementById("midpointValue").textContent = parseFloat(
    e.target.value,
  ).toFixed(2);
  switchToCustomPreset();
  updatePreviews();
});

document.querySelectorAll('input[name="colorMethod"]').forEach((radio) => {
  radio.addEventListener("change", (e) => {
    enhancedParams.colorMethod = e.target.value;
    updatePreviews();
  });
});

document.querySelectorAll('input[name="ditherAlgorithm"]').forEach((radio) => {
  radio.addEventListener("change", (e) => {
    enhancedParams.ditherAlgorithm = e.target.value;
    stockParams.ditherAlgorithm = e.target.value;
    updatePreviews();
  });
});

document.querySelectorAll('input[name="toneMode"]').forEach((radio) => {
  radio.addEventListener("change", (e) => {
    enhancedParams.toneMode = e.target.value;

    const contrastControl = document.getElementById("contrastControl");
    const curveWrapper = document.getElementById("curveWrapper");

    if (e.target.value === "contrast") {
      contrastControl.style.display = "block";
      curveWrapper.style.display = "none";
    } else {
      contrastControl.style.display = "none";
      curveWrapper.style.display = "flex";
    }

    updatePreviews();
  });
});

document.querySelectorAll('input[name="renderPalette"]').forEach((radio) => {
  radio.addEventListener("change", (e) => {
    const useMeasured = e.target.value === "measured";
    enhancedParams.renderMeasured = useMeasured;
    stockParams.renderMeasured = useMeasured;
    updatePreviews();
  });
});

// Compress dynamic range checkbox
document
  .getElementById("compressDynamicRange")
  .addEventListener("change", (e) => {
    enhancedParams.compressDynamicRange = e.target.checked;
    switchToCustomPreset();
    updatePreviews();
  });

// Preset selection
document.querySelectorAll('input[name="preset"]').forEach((radio) => {
  radio.addEventListener("change", (e) => {
    const presetName = e.target.value;

    // Update hint text
    updatePresetHint(presetName);

    if (presetName === "custom") {
      currentPreset = "custom";
      return;
    }

    // Load preset parameters
    const preset = getPreset(presetName);
    if (preset) {
      currentPreset = presetName;
      Object.assign(enhancedParams, preset);
      enhancedParams.renderMeasured = true;

      // Update UI controls
      updateUIFromParams();
      updatePreviews();
    }
  });
});

// Update preset hint text based on selected preset
function updatePresetHint(presetName) {
  const hintText = document.getElementById("presetHintText");
  const hints = {
    cdr: "Prevents overexposure and generates good image quality for all images. Images will have a slightly darker look.",
    scurve:
      "Advanced tone mapping with brighter output. Some parts of the image may be over-exposed.",
    custom: "Manually adjusted parameters",
  };
  hintText.textContent = hints[presetName] || "";
}

// Helper function to switch to custom preset when user manually adjusts parameters
function switchToCustomPreset() {
  if (currentPreset !== "custom") {
    currentPreset = "custom";
    document.querySelector('input[name="preset"][value="custom"]').checked =
      true;
    updatePresetHint("custom");
  }
}

// Helper function to update all UI controls from enhancedParams
function updateUIFromParams() {
  // Update sliders and values
  if (enhancedParams.exposure !== undefined) {
    document.getElementById("exposure").value = enhancedParams.exposure;
    document.getElementById("exposureValue").textContent =
      enhancedParams.exposure.toFixed(2);
  }
  if (enhancedParams.saturation !== undefined) {
    document.getElementById("saturation").value = enhancedParams.saturation;
    document.getElementById("saturationValue").textContent =
      enhancedParams.saturation.toFixed(2);
  }
  if (enhancedParams.contrast !== undefined) {
    document.getElementById("contrast").value = enhancedParams.contrast;
    document.getElementById("contrastValue").textContent =
      enhancedParams.contrast.toFixed(2);
  }

  // S-curve parameters (use defaults if not defined)
  document.getElementById("scurveStrength").value =
    enhancedParams.strength || 0.9;
  document.getElementById("strengthValue").textContent = (
    enhancedParams.strength || 0.9
  ).toFixed(2);
  document.getElementById("scurveShadow").value =
    enhancedParams.shadowBoost || 0.0;
  document.getElementById("shadowValue").textContent = (
    enhancedParams.shadowBoost || 0.0
  ).toFixed(2);
  document.getElementById("scurveHighlight").value =
    enhancedParams.highlightCompress || 1.5;
  document.getElementById("highlightValue").textContent = (
    enhancedParams.highlightCompress || 1.5
  ).toFixed(2);
  document.getElementById("scurveMidpoint").value =
    enhancedParams.midpoint || 0.5;
  document.getElementById("midpointValue").textContent = (
    enhancedParams.midpoint || 0.5
  ).toFixed(2);

  // Checkbox
  if (enhancedParams.compressDynamicRange !== undefined) {
    document.getElementById("compressDynamicRange").checked =
      enhancedParams.compressDynamicRange;
  }

  // Radio buttons
  if (enhancedParams.colorMethod) {
    document.querySelector(
      `input[name="colorMethod"][value="${enhancedParams.colorMethod}"]`,
    ).checked = true;
  }
  if (enhancedParams.toneMode) {
    document.querySelector(
      `input[name="toneMode"][value="${enhancedParams.toneMode}"]`,
    ).checked = true;

    // Update UI visibility based on tone mode
    const contrastControl = document.getElementById("contrastControl");
    const curveWrapper = document.getElementById("curveWrapper");
    if (enhancedParams.toneMode === "contrast") {
      contrastControl.style.display = "block";
      curveWrapper.style.display = "none";
    } else {
      contrastControl.style.display = "none";
      curveWrapper.style.display = "flex";
    }
  }
  if (enhancedParams.ditherAlgorithm) {
    document.querySelector(
      `input[name="ditherAlgorithm"][value="${enhancedParams.ditherAlgorithm}"]`,
    ).checked = true;
  }
}

document.getElementById("resetParams").addEventListener("click", () => {
  // Load CDR preset from image-processor.js (single source of truth)
  const cdrPreset = getPreset("cdr");
  Object.assign(enhancedParams, cdrPreset);

  // Update preset selection
  currentPreset = "cdr";
  document.querySelector('input[name="preset"][value="cdr"]').checked = true;
  updatePresetHint("cdr");

  // Update UI using helper function
  updateUIFromParams();

  // Update previews
  updatePreviews();
});

document.getElementById("newImage").addEventListener("click", () => {
  currentImageFile = null;
  sourceCanvas = null;
  fileInput.value = "";
  document.getElementById("fileName").textContent = "";
  uploadArea.style.display = "block";
  previewSection.style.display = "none";
});

async function loadSampleImage() {
  try {
    const response = await fetch("sample.jpg");
    const blob = await response.blob();
    const file = new File([blob], "sample.jpg", { type: "image/jpeg" });

    currentImageFile = file;
    uploadArea.style.display = "none";
    previewSection.style.display = "block";
    document.getElementById("fileName").textContent = "Sample Image";

    await loadImagePreview(file);
  } catch (error) {
    console.error("Error loading sample image:", error);
  }
}

// Initialize UI with CDR preset parameters
updateUIFromParams();

loadSampleImage();

// Version switching for firmware flasher
function switchVersion(version) {
  const versionWarning = document.getElementById("version-warning");
  const buttonContainer = document.querySelector(".flash-container");

  // Remove old button
  const oldButton = document.getElementById("install-button");
  if (oldButton) {
    oldButton.remove();
  }

  // Create new button with correct manifest
  const manifestPath =
    version === "dev" ? "./manifest-dev.json" : "./manifest.json";
  const newButton = document.createElement("esp-web-install-button");
  newButton.id = "install-button";
  newButton.setAttribute("manifest", manifestPath);

  const button = document.createElement("button");
  button.setAttribute("slot", "activate");
  button.textContent = "Connect & Flash";
  newButton.appendChild(button);

  // Insert before warning message
  buttonContainer.insertBefore(newButton, versionWarning);

  // Show/hide warning
  if (version === "dev") {
    versionWarning.style.display = "block";
  } else {
    versionWarning.style.display = "none";
  }
}

// Load version info for both manifests
async function loadVersionInfo() {
  const select = document.getElementById("version-select");
  // Add timestamp to bypass cache
  const timestamp = new Date().getTime();

  try {
    // Fetch stable version with cache-busting
    const stableResponse = await fetch(`./manifest.json?t=${timestamp}`);
    const stableData = await stableResponse.json();
    select.options[0].text = `Stable Release (${stableData.version})`;
  } catch (e) {
    console.error("Failed to load stable version:", e);
  }

  try {
    // Fetch dev version with cache-busting
    const devResponse = await fetch(`./manifest-dev.json?t=${timestamp}`);
    const devData = await devResponse.json();
    select.options[1].text = `Development (${devData.version})`;
  } catch (e) {
    console.error("Failed to load dev version:", e);
  }
}

// Load version info on page load
loadVersionInfo();

// Initialize the button with the correct manifest on page load
window.addEventListener("DOMContentLoaded", () => {
  const select = document.getElementById("version-select");

  // Attach change event listener
  select.addEventListener("change", (e) => {
    switchVersion(e.target.value);
  });

  // Trigger switchVersion with the current selection to ensure correct manifest
  switchVersion(select.value);
});
