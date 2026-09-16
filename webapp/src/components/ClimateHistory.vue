<script setup>
import { ref, computed, onMounted } from "vue";
import { useSettingsStore } from "../stores";
import { formatTimeAxisTick } from "../utils/chartAxis";

const settingsStore = useSettingsStore();

const API_BASE = "";

const loading = ref(true);
const resetting = ref(false);
const confirmingReset = ref(false);
const savingBackupSetting = ref(false);
const entries = ref([]); // [{ t, temp_c, hum, tcat: 0/1/2, hcat: 0/1/2 }]

async function onBackupToggle() {
  savingBackupSetting.value = true;
  try {
    await settingsStore.saveDeviceSettings();
  } finally {
    savingBackupSetting.value = false;
  }
}

async function loadHistory() {
  loading.value = true;
  try {
    const response = await fetch(`${API_BASE}/api/climate-history`);
    if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
      return;
    }
    const data = await response.json();
    entries.value = data.entries || [];
  } catch (_error) {
    console.log("Climate history not available (standalone mode)");
  } finally {
    loading.value = false;
  }
}

async function resetHistory() {
  resetting.value = true;
  try {
    await fetch(`${API_BASE}/api/climate-history`, { method: "DELETE" });
    await loadHistory();
  } catch (_error) {
    console.log("Failed to reset climate history");
  } finally {
    resetting.value = false;
    confirmingReset.value = false;
  }
}

onMounted(loadHistory);

// Bad=red, Good=orange, Super=green - real orange here (browser CSS), no
// hardware palette constraint like the e-paper badges/Agenda chips have.
const CATEGORY_COLORS = ["#e53935", "#fb8c00", "#43a047"];
function categoryColor(cat) {
  return CATEGORY_COLORS[cat] ?? CATEGORY_COLORS[1];
}

// --- Chart geometry (plain SVG, no charting library - same approach as
// BatteryHistory.vue) - two stacked mini-charts sharing one time axis. ---
const CHART_WIDTH = 800;
const CHART_HEIGHT = 220;
const PAD = { top: 20, right: 24, bottom: 36, left: 44 };
const plotWidth = CHART_WIDTH - PAD.left - PAD.right;
const plotHeight = CHART_HEIGHT - PAD.top - PAD.bottom;

const timeRange = computed(() => {
  if (entries.value.length === 0) return [0, 1];
  const times = entries.value.map((e) => e.t);
  const min = Math.min(...times);
  const max = Math.max(...times);
  return [min, max === min ? min + 1 : max];
});

function xForTime(t) {
  const [min, max] = timeRange.value;
  return PAD.left + ((t - min) / (max - min)) * plotWidth;
}

// Range (with headroom) and the scale function are split so the same range
// can also drive a set of Y-axis ticks (below) without duplicating or
// drifting from the headroom math the plotted points themselves use.
function computeYRange(values) {
  const lo = Math.min(...values);
  const hi = Math.max(...values);
  // A little headroom above/below so points never sit flush on the edge.
  const span = hi - lo || 1;
  return { min: lo - span * 0.1, max: hi + span * 0.1 };
}

function yScaleFromRange(range) {
  return (v) => PAD.top + plotHeight - ((v - range.min) / (range.max - range.min)) * plotHeight;
}

const tempRange = computed(() =>
  entries.value.length ? computeYRange(entries.value.map((e) => e.temp_c)) : { min: 0, max: 1 }
);
const humRange = computed(() =>
  entries.value.length ? computeYRange(entries.value.map((e) => e.hum)) : { min: 0, max: 1 }
);

const yForTemp = computed(() => yScaleFromRange(tempRange.value));
const yForHum = computed(() => yScaleFromRange(humRange.value));

const Y_TICK_COUNT = 5;
function ticksForRange(range) {
  const ticks = [];
  for (let i = 0; i < Y_TICK_COUNT; i++) {
    ticks.push(range.min + ((range.max - range.min) * i) / (Y_TICK_COUNT - 1));
  }
  return ticks;
}

const tempYTicks = computed(() => ticksForRange(tempRange.value));
const humYTicks = computed(() => ticksForRange(humRange.value));

function formatTempTick(c) {
  if (settingsStore.deviceSettings.climateTempUnit === "fahrenheit") {
    return `${Math.round((c * 9) / 5 + 32)}°F`;
  }
  return `${Math.round(c * 10) / 10}°C`;
}
function formatHumTick(h) {
  return `${Math.round(h)}%`;
}

const tempLinePoints = computed(() =>
  entries.value
    .map((e) => `${xForTime(e.t).toFixed(1)},${yForTemp.value(e.temp_c).toFixed(1)}`)
    .join(" ")
);
const humLinePoints = computed(() =>
  entries.value
    .map((e) => `${xForTime(e.t).toFixed(1)},${yForHum.value(e.hum).toFixed(1)}`)
    .join(" ")
);

// A handful of evenly-spaced date labels along the x-axis, same approach as
// BatteryHistory.vue (avoids clutter with potentially hundreds of points).
const xTicks = computed(() => {
  if (entries.value.length === 0) return [];
  const [min, max] = timeRange.value;
  const count = 5;
  const ticks = [];
  for (let i = 0; i < count; i++) {
    const t = min + ((max - min) * i) / (count - 1);
    ticks.push({
      x: xForTime(t),
      label: formatTimeAxisTick(t, max - min),
    });
  }
  return ticks;
});

function pointTitle(e, kind) {
  const date = new Date(e.t * 1000).toLocaleString();
  return kind === "temp" ? `${date}\n${e.temp_c}°C` : `${date}\n${e.hum}%`;
}
</script>

<template>
  <v-card>
    <v-card-title class="d-flex align-center">
      <v-icon icon="mdi-thermometer" class="mr-2" />
      Climate History
      <v-spacer />
      <v-switch
        v-model="settingsStore.deviceSettings.climateHistoryBackupEnabled"
        :loading="savingBackupSetting"
        color="primary"
        density="compact"
        hide-details
        class="flex-grow-0 mr-2"
        @update:model-value="onBackupToggle"
      >
        <template #label>
          <span class="text-caption">Auto-backup to SD</span>
        </template>
      </v-switch>
      <v-btn
        v-if="entries.length > 0"
        variant="text"
        size="small"
        color="error"
        @click="confirmingReset = true"
      >
        <v-icon icon="mdi-delete-outline" start />
        Reset
      </v-btn>
    </v-card-title>

    <v-card-text>
      <div v-if="loading" class="d-flex justify-center align-center py-12">
        <v-progress-circular indeterminate color="primary" />
      </div>

      <v-alert v-else-if="entries.length === 0" type="info" variant="tonal">
        No climate history recorded yet. A reading is saved once per successfully displayed image
        (requires the SHTC3 sensor to respond, logging enabled in the Climate settings tab, and
        persistent storage to be mounted).
      </v-alert>

      <template v-else>
        <div class="text-caption text-medium-emphasis mb-2">
          {{ entries.length }} reading{{ entries.length === 1 ? "" : "s" }} - point color shows the
          category for the currently selected room type (Climate settings tab): red = Bad, orange =
          Good, green = Super.
        </div>

        <div class="text-body-2 mb-1">Temperature</div>
        <svg
          :viewBox="`0 0 ${CHART_WIDTH} ${CHART_HEIGHT}`"
          preserveAspectRatio="xMidYMid meet"
          style="width: 100%; height: auto; max-height: 260px"
        >
          <g v-for="(tick, i) in tempYTicks" :key="'yt' + i">
            <line
              :x1="PAD.left"
              :x2="CHART_WIDTH - PAD.right"
              :y1="yForTemp(tick)"
              :y2="yForTemp(tick)"
              stroke="currentColor"
              stroke-opacity="0.12"
            />
            <text
              :x="PAD.left - 8"
              :y="yForTemp(tick) + 4"
              text-anchor="end"
              font-size="11"
              fill="currentColor"
              fill-opacity="0.6"
            >
              {{ formatTempTick(tick) }}
            </text>
          </g>
          <text
            v-for="tick in xTicks"
            :key="'xt' + tick.label + tick.x"
            :x="tick.x"
            :y="CHART_HEIGHT - PAD.bottom + 20"
            text-anchor="middle"
            font-size="11"
            fill="currentColor"
            fill-opacity="0.6"
          >
            {{ tick.label }}
          </text>
          <polyline :points="tempLinePoints" fill="none" stroke="#1976d2" stroke-width="2" />
          <circle
            v-for="(e, i) in entries"
            :key="i"
            :cx="xForTime(e.t)"
            :cy="yForTemp(e.temp_c)"
            r="3"
            :fill="categoryColor(e.tcat)"
          >
            <title>{{ pointTitle(e, "temp") }}</title>
          </circle>
        </svg>

        <div class="text-body-2 mb-1 mt-2">Humidity</div>
        <svg
          :viewBox="`0 0 ${CHART_WIDTH} ${CHART_HEIGHT}`"
          preserveAspectRatio="xMidYMid meet"
          style="width: 100%; height: auto; max-height: 260px"
        >
          <g v-for="(tick, i) in humYTicks" :key="'yh' + i">
            <line
              :x1="PAD.left"
              :x2="CHART_WIDTH - PAD.right"
              :y1="yForHum(tick)"
              :y2="yForHum(tick)"
              stroke="currentColor"
              stroke-opacity="0.12"
            />
            <text
              :x="PAD.left - 8"
              :y="yForHum(tick) + 4"
              text-anchor="end"
              font-size="11"
              fill="currentColor"
              fill-opacity="0.6"
            >
              {{ formatHumTick(tick) }}
            </text>
          </g>
          <text
            v-for="tick in xTicks"
            :key="'xh' + tick.label + tick.x"
            :x="tick.x"
            :y="CHART_HEIGHT - PAD.bottom + 20"
            text-anchor="middle"
            font-size="11"
            fill="currentColor"
            fill-opacity="0.6"
          >
            {{ tick.label }}
          </text>
          <polyline :points="humLinePoints" fill="none" stroke="#1976d2" stroke-width="2" />
          <circle
            v-for="(e, i) in entries"
            :key="i"
            :cx="xForTime(e.t)"
            :cy="yForHum(e.hum)"
            r="3"
            :fill="categoryColor(e.hcat)"
          >
            <title>{{ pointTitle(e, "hum") }}</title>
          </circle>
        </svg>

        <div class="d-flex align-center ga-4 mt-2">
          <div class="d-flex align-center ga-1">
            <span class="legend-dot" style="background: #e53935"></span>
            <span class="text-caption text-medium-emphasis">Bad</span>
          </div>
          <div class="d-flex align-center ga-1">
            <span class="legend-dot" style="background: #fb8c00"></span>
            <span class="text-caption text-medium-emphasis">Good</span>
          </div>
          <div class="d-flex align-center ga-1">
            <span class="legend-dot" style="background: #43a047"></span>
            <span class="text-caption text-medium-emphasis">Super</span>
          </div>
        </div>

        <div class="text-caption text-medium-emphasis mt-4">
          One reading is recorded after each image change, while logging is enabled. The history
          resets automatically after 180 days, or any time via the button above. With "Auto-backup
          to SD" on (default), the 180-day reset saves a copy of the discarded readings to storage
          first, named after the date range it covers.
        </div>
      </template>
    </v-card-text>

    <v-dialog v-model="confirmingReset" max-width="440">
      <v-card>
        <v-card-title class="text-error">
          <v-icon icon="mdi-alert" class="mr-2" />
          Reset Climate History?
        </v-card-title>
        <v-card-text>
          This permanently deletes all recorded temperature/humidity readings. This cannot be
          undone.
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="confirmingReset = false">Cancel</v-btn>
          <v-btn color="error" variant="flat" :loading="resetting" @click="resetHistory">
            Reset
          </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>
  </v-card>
</template>

<style scoped>
.legend-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  display: inline-block;
}
</style>
