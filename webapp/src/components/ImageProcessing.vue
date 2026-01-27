<script setup>
import { ref, watch, onMounted, onUnmounted } from "vue";
import ToneCurve from "./ToneCurve.vue";

const props = defineProps({
  imageFile: {
    type: File,
    default: null,
  },
  params: {
    type: Object,
    required: true,
  },
  palette: {
    type: Object,
    default: null,
  },
});

const emit = defineEmits(["processed"]);

// Canvas refs
const originalCanvasRef = ref(null);
const processedCanvasRef = ref(null);

// State
const processing = ref(false);
const sliderPosition = ref(0);
const isDragging = ref(false);

// Display dimensions
const DISPLAY_WIDTH = 800;
const DISPLAY_HEIGHT = 480;

// Source canvas for reprocessing
let sourceCanvas = null;
let imageProcessor = null;
let isReady = ref(false);

onMounted(async () => {
  // Load the image processing library
  try {
    imageProcessor = await import("epaper-image-convert");
    isReady.value = true;
    // Process if file was already set
    if (props.imageFile) {
      await loadAndProcessImage(props.imageFile);
    }
  } catch (error) {
    console.error("Failed to load image processor:", error);
  }
});

// Watch for image file changes
watch(
  () => props.imageFile,
  async (file) => {
    if (file && isReady.value) {
      await loadAndProcessImage(file);
    }
  }
);

// Watch for parameter changes - reprocess without reloading image
watch(
  () => props.params,
  async () => {
    if (sourceCanvas && isReady.value) {
      await updatePreview();
    }
  },
  { deep: true }
);

async function loadAndProcessImage(file) {
  if (!originalCanvasRef.value || !processedCanvasRef.value) return;

  processing.value = true;

  try {
    // Load image with EXIF orientation applied
    const img = await loadImage(file);

    // Create source canvas from loaded image
    sourceCanvas = document.createElement("canvas");
    sourceCanvas.width = img.width;
    sourceCanvas.height = img.height;
    const sourceCtx = sourceCanvas.getContext("2d");
    sourceCtx.drawImage(img, 0, 0);

    // Process and display
    await updatePreview();
  } catch (error) {
    console.error("Image loading failed:", error);
  } finally {
    processing.value = false;
  }
}

async function updatePreview() {
  if (!sourceCanvas || !originalCanvasRef.value || !processedCanvasRef.value || !imageProcessor)
    return;

  const processingParams = {
    exposure: props.params.exposure,
    saturation: props.params.saturation,
    toneMode: props.params.toneMode,
    contrast: props.params.contrast,
    strength: props.params.strength,
    shadowBoost: props.params.shadowBoost,
    highlightCompress: props.params.highlightCompress,
    midpoint: props.params.midpoint,
    colorMethod: props.params.colorMethod,
    ditherAlgorithm: props.params.ditherAlgorithm,
    compressDynamicRange: props.params.compressDynamicRange,
  };

  // Build palette for processing
  const palette = imageProcessor.SPECTRA6;
  if (props.palette && Object.keys(props.palette).length > 0) {
    palette.perceived = props.palette;
  }

  // Determine if image is portrait (height > width)
  const isPortrait = sourceCanvas.height > sourceCanvas.width;
  const displayWidth = isPortrait ? DISPLAY_HEIGHT : DISPLAY_WIDTH; // 480 or 800
  const displayHeight = isPortrait ? DISPLAY_WIDTH : DISPLAY_HEIGHT; // 800 or 480

  // Process image with dithering for preview
  // Use skipRotation: true to display portrait images in portrait mode
  const result = imageProcessor.processImage(sourceCanvas, {
    displayWidth: isPortrait ? DISPLAY_HEIGHT : DISPLAY_WIDTH,
    displayHeight: isPortrait ? DISPLAY_WIDTH : DISPLAY_HEIGHT,
    palette,
    params: processingParams,
    skipRotation: true,
    usePerceivedOutput: true,
  });

  // Set canvas dimensions to exact display size
  originalCanvasRef.value.width = displayWidth;
  originalCanvasRef.value.height = displayHeight;
  processedCanvasRef.value.width = displayWidth;
  processedCanvasRef.value.height = displayHeight;

  // Draw original using cover mode (same as old webapp)
  const originalResized = imageProcessor.resizeImageCover(
    sourceCanvas,
    displayWidth,
    displayHeight
  );
  const originalCtx = originalCanvasRef.value.getContext("2d");
  originalCtx.drawImage(originalResized, 0, 0);

  // Draw processed result
  const processedCtx = processedCanvasRef.value.getContext("2d");
  processedCtx.drawImage(result.canvas, 0, 0, displayWidth, displayHeight);

  emit("processed", result);
}

function loadImage(file) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = reject;
    img.src = URL.createObjectURL(file);
  });
}

// Slider drag handling
function startDrag(event) {
  isDragging.value = true;
  updateSlider(event);
}

function onDrag(event) {
  if (isDragging.value) {
    updateSlider(event);
  }
}

function stopDrag() {
  isDragging.value = false;
}

function updateSlider(event) {
  const container = event.currentTarget;
  const rect = container.getBoundingClientRect();
  const x = event.clientX - rect.left;
  sliderPosition.value = Math.max(0, Math.min(100, (x / rect.width) * 100));
}

onUnmounted(() => {
  // Cleanup
});
</script>

<template>
  <v-card>
    <v-card-text>
      <div class="d-flex flex-column align-center">
        <div class="d-flex flex-wrap gap-4 justify-center align-end">
          <!-- Comparison Slider -->
          <div
            class="comparison-container"
            @mousedown="startDrag"
            @mousemove="onDrag"
            @mouseup="stopDrag"
            @mouseleave="stopDrag"
          >
            <div class="canvas-wrapper">
              <canvas ref="originalCanvasRef" class="preview-canvas" />
              <canvas
                ref="processedCanvasRef"
                class="preview-canvas processed"
                :style="{ clipPath: `inset(0 0 0 ${sliderPosition}%)` }"
              />
              <div class="slider-line" :style="{ left: `${sliderPosition}%` }">
                <div class="slider-handle">
                  <v-icon size="small"> mdi-arrow-left-right </v-icon>
                </div>
              </div>
            </div>
            <div class="comparison-labels d-flex justify-space-between mt-2">
              <span class="text-caption">← Original</span>
              <span class="text-caption">Processed →</span>
            </div>
          </div>

          <!-- Tone Curve -->
          <v-card variant="outlined" class="tone-curve-card">
            <v-card-subtitle class="pt-2"> Tone Curve </v-card-subtitle>
            <div class="d-flex justify-center pa-4">
              <ToneCurve :params="params" class="curve-canvas" />
            </div>
          </v-card>
        </div>

        <v-progress-linear v-if="processing" indeterminate color="primary" class="mt-2" />
      </div>
    </v-card-text>
  </v-card>
</template>

<style scoped>
.comparison-container {
  position: relative;
  cursor: ew-resize;
  user-select: none;
}

.canvas-wrapper {
  position: relative;
  display: inline-block;
  background: #f5f5f5;
  border-radius: 8px;
  overflow: hidden;
}

.preview-canvas {
  display: block;
}

.preview-canvas.processed {
  position: absolute;
  top: 0;
  left: 0;
  z-index: 1;
}

.slider-line {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 3px;
  background: white;
  z-index: 2;
  transform: translateX(-50%);
  box-shadow: 0 0 4px rgba(0, 0, 0, 0.3);
}

.slider-handle {
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  width: 32px;
  height: 32px;
  background: white;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
}

.curve-canvas {
  border: 1px solid #e0e0e0;
  border-radius: 4px;
}

.tone-curve-card {
  flex-shrink: 0;
  align-self: flex-end;
  margin-left: 20px;
}
</style>
