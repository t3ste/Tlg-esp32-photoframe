<script setup>
import { ref, computed, onUnmounted } from "vue";

const props = defineProps({
  // Speaker on the same board: enables the speaker + microphone self-test.
  speakerAvailable: { type: Boolean, default: false },
});
const emit = defineEmits(["message"]);

const MIN_DB = -90;
const MAX_DB = 0;
const POLL_MS = 250;
const LIVE_SECONDS = 60;

const live = ref(false);
const status = ref(null);
const settings = ref({ auto: true, threshold_dbfs: -45, auto_rise_db: 20, auto_min_dbfs: -45 });
const thresholdSlider = ref(-45);
const savingSettings = ref(false);
const testingMic = ref(false);
const testingMicTones = ref(false);
const selfTestResult = ref(null);

let pollTimer = null;
let polling = false;

const mic1 = computed(() => status.value?.mic_dbfs ?? MIN_DB);
const mic2 = computed(() => status.value?.mic2_dbfs ?? MIN_DB);
const floorDb = computed(() => status.value?.floor_dbfs ?? MIN_DB);
const threshold = computed(() =>
  settings.value.auto
    ? (status.value?.threshold_dbfs ?? settings.value.auto_min_dbfs)
    : thresholdSlider.value
);

function pct(db) {
  const clamped = Math.min(MAX_DB, Math.max(MIN_DB, db));
  return ((clamped - MIN_DB) / (MAX_DB - MIN_DB)) * 100;
}

function fmt(db) {
  return `${db <= MIN_DB ? "<-90" : db.toFixed(1)} dBFS`;
}

const channels = computed(() => [
  { name: "Microphone 1", level: mic1.value },
  { name: "Microphone 2", level: mic2.value },
]);

const anyAbove = computed(() => live.value && Math.max(mic1.value, mic2.value) > threshold.value);

async function loadSettings() {
  try {
    const response = await fetch("/api/mic/settings");
    if (!response.ok) return;
    settings.value = await response.json();
    thresholdSlider.value = settings.value.threshold_dbfs;
  } catch (error) {
    console.error("Failed to load microphone settings:", error);
  }
}

async function saveSettings() {
  savingSettings.value = true;
  try {
    const response = await fetch("/api/mic/settings", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        auto: settings.value.auto,
        threshold_dbfs: Math.round(thresholdSlider.value),
      }),
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    settings.value = await response.json();
  } catch (_error) {
    emit("message", { text: "Failed to save the microphone threshold", color: "error" });
    await loadSettings();
  } finally {
    savingSettings.value = false;
  }
}

async function poll() {
  if (polling) return;
  polling = true;
  try {
    const response = await fetch("/api/mic/level");
    if (!response.ok) return;
    status.value = await response.json();
    // The device listens for at most a minute per start: keep it going while the meter is on.
    if (live.value && !status.value.running && !status.value.tones_running) {
      await fetch(`/api/mic/level?seconds=${LIVE_SECONDS}`, { method: "POST" });
    }
  } catch (_error) {
    // transient network error: try again on the next tick
  } finally {
    polling = false;
  }
}

function startPolling() {
  if (!pollTimer) pollTimer = setInterval(poll, POLL_MS);
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

async function stopDeviceMonitor() {
  try {
    await fetch("/api/mic/level", { method: "DELETE" });
  } catch (_error) {
    // ignore
  }
}

async function setLive(on) {
  live.value = on;
  if (on) {
    await loadSettings();
    selfTestResult.value = null;
    await poll();
    startPolling();
  } else {
    stopPolling();
    await stopDeviceMonitor();
  }
}

async function waitUntilIdle() {
  for (let i = 0; i < 20; i++) {
    const s = await fetch("/api/mic/level").then((r) => r.json());
    if (!s.running && !s.tones_running) return true;
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  return false;
}

async function testMicrophone() {
  testingMic.value = true;
  try {
    await setLive(false);
    await waitUntilIdle();
    const response = await fetch("/api/mic/level?seconds=15", { method: "POST" });
    const data = await response.json().catch(() => ({}));
    emit(
      "message",
      response.ok
        ? {
            text: `Listening for ${data.seconds} s - watch the device console or the debug log`,
            color: "success",
          }
        : { text: data.error || "Failed to start the microphone test", color: "error" }
    );
  } catch (_error) {
    emit("message", { text: "Failed to start the microphone test", color: "error" });
  } finally {
    setTimeout(() => (testingMic.value = false), 3000);
  }
}

// Speaker + microphone self-test on this frame: plays a tone sequence at 100 %
// volume on its own speaker while its microphone listens; the device counts the
// tone bursts against the threshold above.
async function runSelfTest() {
  testingMicTones.value = true;
  selfTestResult.value = null;
  try {
    await setLive(false);
    await waitUntilIdle();
    const response = await fetch("/api/mic/level?seconds=8&tones=1", { method: "POST" });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      emit("message", { text: data.error || "Failed to start the self-test", color: "error" });
      return;
    }
    for (let i = 0; i < 20; i++) {
      await new Promise((resolve) => setTimeout(resolve, 1000));
      const s = await fetch("/api/mic/level").then((r) => r.json());
      if (!s.running && s.result) {
        selfTestResult.value = s.result;
        return;
      }
    }
    emit("message", { text: "The self-test did not finish in time", color: "error" });
  } catch (_error) {
    emit("message", { text: "Failed to run the self-test", color: "error" });
  } finally {
    testingMicTones.value = false;
  }
}

onUnmounted(() => {
  stopPolling();
  if (live.value) stopDeviceMonitor();
});

loadSettings();
</script>

<template>
  <div>
    <div class="text-subtitle-1 mb-4">Microphone</div>

    <v-switch
      :model-value="live"
      color="primary"
      density="compact"
      hide-details
      label="Live level"
      @update:model-value="setLive"
    />
    <div class="text-caption text-medium-emphasis mb-3">
      The frame listens while this is on (it stays awake and uses more power) and shows the input
      level below. Nothing is recorded or stored.
    </div>

    <div v-for="ch in channels" :key="ch.name" class="mic-channel">
      <div class="d-flex justify-space-between text-caption">
        <span>{{ ch.name }}</span>
        <span>{{ live ? fmt(ch.level) : "-" }}</span>
      </div>
      <div class="mic-track">
        <div
          class="mic-fill"
          :class="{ 'mic-fill-hot': live && ch.level > threshold }"
          :style="{ width: live ? pct(ch.level) + '%' : '0%' }"
        />
        <div
          v-if="live"
          class="mic-marker mic-marker-floor"
          :style="{ left: pct(floorDb) + '%' }"
          title="Noise floor"
        />
        <div
          class="mic-marker mic-marker-threshold"
          :style="{ left: pct(threshold) + '%' }"
          title="Threshold"
        />
      </div>
    </div>
    <div class="mic-scale text-caption text-medium-emphasis">
      <span>-90</span><span>-60</span><span>-30</span><span>0 dBFS</span>
    </div>
    <div class="d-flex flex-wrap ga-4 text-caption mt-1">
      <span
        ><span class="mic-swatch mic-marker-floor" /> Noise floor:
        {{ live ? fmt(floorDb) : "-" }}</span
      >
      <span
        ><span class="mic-swatch mic-marker-threshold" /> Threshold: {{ fmt(threshold) }}
        <span v-if="settings.auto">(automatic)</span></span
      >
      <span v-if="live" :class="anyAbove ? 'text-success' : 'text-medium-emphasis'">
        {{ anyAbove ? "above threshold" : "below threshold" }}
      </span>
    </div>

    <v-switch
      v-model="settings.auto"
      color="primary"
      density="compact"
      hide-details
      class="mt-3"
      :loading="savingSettings"
      :label="`Automatic threshold (noise floor + ${settings.auto_rise_db} dB, at least ${settings.auto_min_dbfs} dBFS)`"
      @update:model-value="saveSettings"
    />
    <v-slider
      v-model="thresholdSlider"
      :min="MIN_DB"
      :max="MAX_DB"
      :step="1"
      :disabled="settings.auto"
      thumb-label
      hide-details
      density="compact"
      color="error"
      class="mt-2"
      @end="saveSettings"
    >
      <template #thumb-label="{ modelValue }">{{ Math.round(modelValue) }}</template>
      <template #prepend><span class="text-caption">Threshold</span></template>
      <template #append
        ><span class="text-caption">{{ Math.round(thresholdSlider) }} dBFS</span></template
      >
    </v-slider>
    <div class="text-caption text-medium-emphasis mt-1">
      Turn Live on in the quiet room and read the noise floor, then set the threshold clearly above
      it (about +20 dB) but below the quietest sound you still want to detect. The self-test and
      other listeners count every rise above the threshold as one sound.
    </div>

    <div class="d-flex flex-wrap ga-2 mt-4">
      <v-btn
        variant="outlined"
        :loading="testingMic"
        title="Prints the input level to the debug log for 15 seconds"
        @click="testMicrophone"
      >
        <v-icon start>mdi-microphone</v-icon>
        Log level
      </v-btn>
      <v-btn
        v-if="props.speakerAvailable"
        variant="outlined"
        :loading="testingMicTones"
        title="Plays tones on the speaker and checks that the microphone hears them"
        @click="runSelfTest"
      >
        <v-icon start>mdi-volume-high</v-icon>
        Self-test
      </v-btn>
    </div>
    <div>
      <v-alert
        v-if="selfTestResult"
        :type="selfTestResult.heard ? 'success' : 'warning'"
        variant="tonal"
        density="compact"
        class="mt-3"
      >
        {{
          selfTestResult.heard
            ? "The microphone hears the speaker."
            : "The microphone did not clearly hear the tones."
        }}
        Microphone: {{ selfTestResult.mic_bursts }}/{{ selfTestResult.expected_bursts }} tone
        bursts, peak {{ selfTestResult.mic_peak_dbfs }} dBFS over a noise floor of
        {{ selfTestResult.baseline_dbfs }} dBFS (threshold {{ selfTestResult.threshold_dbfs }}
        dBFS).
      </v-alert>
    </div>
  </div>
</template>

<style scoped>
.mic-channel {
  margin-top: 6px;
}
.mic-track {
  position: relative;
  height: 22px;
  border-radius: 4px;
  overflow: hidden;
  background: rgba(var(--v-theme-on-surface), 0.12);
}
.mic-fill {
  height: 100%;
  background: rgb(var(--v-theme-primary));
  transition: width 0.2s linear;
}
.mic-fill-hot {
  background: rgb(var(--v-theme-success));
}
.mic-marker {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 3px;
  transform: translateX(-1px);
}
.mic-marker-floor {
  background: rgb(var(--v-theme-info));
}
.mic-marker-threshold {
  background: rgb(var(--v-theme-error));
}
.mic-scale {
  display: flex;
  justify-content: space-between;
  margin-top: 2px;
}
.mic-swatch {
  display: inline-block;
  width: 10px;
  height: 10px;
  margin-right: 4px;
  border-radius: 2px;
}
</style>
