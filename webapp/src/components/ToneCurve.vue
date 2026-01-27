<script setup>
import { ref, onMounted, watch, nextTick, useAttrs } from "vue";

defineOptions({ inheritAttrs: false });

const props = defineProps({
  params: {
    type: Object,
    required: true,
  },
  size: {
    type: Number,
    default: 200,
  },
});

const attrs = useAttrs();
const canvasRef = ref(null);

function drawToneCurve() {
  if (!canvasRef.value) return;

  const canvas = canvasRef.value;
  const ctx = canvas.getContext("2d");
  const size = props.size;

  canvas.width = size;
  canvas.height = size;

  ctx.fillStyle = "#f5f5f5";
  ctx.fillRect(0, 0, size, size);

  ctx.strokeStyle = "#e0e0e0";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const pos = (i / 4) * size;
    ctx.beginPath();
    ctx.moveTo(pos, 0);
    ctx.lineTo(pos, size);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(0, pos);
    ctx.lineTo(size, pos);
    ctx.stroke();
  }

  ctx.strokeStyle = "#bdbdbd";
  ctx.beginPath();
  ctx.moveTo(0, size);
  ctx.lineTo(size, 0);
  ctx.stroke();

  ctx.strokeStyle = "#1976D2";
  ctx.lineWidth = 2;
  ctx.beginPath();

  const params = props.params;

  for (let x = 0; x <= size; x++) {
    const input = x / size;
    let output;

    if (params.toneMode === "scurve") {
      const strength = params.strength;
      const midpoint = params.midpoint;
      const shadowBoost = params.shadowBoost;
      const highlightCompress = params.highlightCompress;

      if (strength === 0) {
        output = input;
      } else if (input <= midpoint) {
        const shadowVal = input / midpoint;
        output = Math.pow(shadowVal, 1.0 - strength * shadowBoost) * midpoint;
      } else {
        const highlightVal = (input - midpoint) / (1.0 - midpoint);
        output =
          midpoint + Math.pow(highlightVal, 1.0 + strength * highlightCompress) * (1.0 - midpoint);
      }
    } else {
      const contrast = params.contrast;
      output = ((input * 255.0 - 128.0) * contrast + 128.0) / 255.0;
    }

    output = Math.max(0, Math.min(1, output));
    const y = size - output * size;

    if (x === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }

  ctx.stroke();
}

function scheduleDraw() {
  requestAnimationFrame(() => drawToneCurve());
}

onMounted(async () => {
  await nextTick();
  scheduleDraw();
});

watch(
  () => props.params,
  async () => {
    await nextTick();
    scheduleDraw();
  },
  { deep: true }
);
</script>

<template>
  <canvas ref="canvasRef" v-bind="attrs" />
</template>
