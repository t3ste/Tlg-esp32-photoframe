<script setup>
import { ref, computed, watch, onUnmounted } from "vue";

const props = defineProps({
  // Speaker and microphone on this board (an Alarm Clock build): the stop word part is shown.
  voiceAvailable: { type: Boolean, default: false },
  // The Alarm Clock tab is showing; the device is only polled then.
  active: { type: Boolean, default: false },
});
const emit = defineEmits(["message"]);

const ENROLL_SECONDS = 3;
const TEST_SECONDS = 10;

const status = ref(null);
const busy = ref(false);
const savingSwitch = ref(false);
const enrollResult = ref(null);
const alarmRinging = ref(false);
const lastStop = ref("");

const POLL_IDLE_MS = 5000;
const POLL_BUSY_MS = 1000;
let pollTimer = null;

const templates = computed(() => status.value?.templates ?? 0);
const maxTemplates = computed(() => status.value?.max_templates ?? 5);
const mode = computed(() => status.value?.mode ?? "idle");
const alarmStop = computed(() => status.value?.alarm_stop === true);
const thresholdMin = computed(() => status.value?.threshold_min ?? 2);
const thresholdMax = computed(() => status.value?.threshold_max ?? 30);
const thresholdAuto = computed(() => !(status.value?.threshold_manual > 0));
const effectiveThreshold = computed(() => status.value?.threshold ?? 4);
const thresholdSlider = ref(4);
const savingThreshold = ref(false);
let draggingThreshold = false;

function roundHalf(v) {
  return Math.round(v * 2) / 2;
}

watch(effectiveThreshold, (t) => {
  if (!draggingThreshold) thresholdSlider.value = roundHalf(t);
});

const stopReasonText = {
  timeout: "it rang until the ring duration ran out",
  key: "it was stopped with the KEY button",
  voice: "it was stopped by the stop word",
  api: "it was stopped from this page",
};

async function loadStatus() {
  try {
    const response = await fetch("/api/kws/status");
    if (response.ok) status.value = await response.json();
  } catch (_error) {
    // transient network error: the next poll tries again
  }
}

async function loadAlarm() {
  try {
    const response = await fetch("/api/alarm/test");
    if (!response.ok) return;
    const data = await response.json();
    alarmRinging.value = data.ringing === true;
    lastStop.value = data.last_stop || "";
  } catch (_error) {
    // ignore
  }
}

async function tick() {
  await Promise.all([props.voiceAvailable ? loadStatus() : null, loadAlarm()]);
}

// Quick while something is running, slow otherwise: the device serves one request at a time.
function schedulePoll() {
  const busyNow = busy.value || alarmRinging.value || mode.value !== "idle";
  pollTimer = setTimeout(
    async () => {
      await tick();
      if (pollTimer) schedulePoll();
    },
    busyNow ? POLL_BUSY_MS : POLL_IDLE_MS
  );
}

function startPolling() {
  if (pollTimer) return;
  tick();
  schedulePoll();
}

function stopPolling() {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = null;
}

watch(
  () => props.active,
  (on) => (on ? startPolling() : stopPolling()),
  { immediate: true }
);

async function callApi(url, options, failText) {
  try {
    const response = await fetch(url, options);
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      emit("message", { text: data.error || failText, color: "error" });
      return null;
    }
    return data;
  } catch (_error) {
    emit("message", { text: failText, color: "error" });
    return null;
  }
}

async function setAlarmStop(on) {
  savingSwitch.value = true;
  try {
    await callApi(
      "/api/kws/settings",
      {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ alarm_stop: on }),
      },
      "Failed to save the setting"
    );
  } finally {
    await loadStatus();
    savingSwitch.value = false;
  }
}

async function saveThreshold(value) {
  savingThreshold.value = true;
  try {
    await callApi(
      "/api/kws/settings",
      {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ threshold: value }),
      },
      "Failed to save the threshold"
    );
  } finally {
    draggingThreshold = false;
    await loadStatus();
    savingThreshold.value = false;
  }
}

function onThresholdAuto(auto) {
  if (auto) {
    saveThreshold(null);
  } else {
    const v = Math.min(
      thresholdMax.value,
      Math.max(thresholdMin.value, roundHalf(effectiveThreshold.value))
    );
    saveThreshold(v);
  }
}

async function waitWhile(modeName, timeoutMs) {
  const until = Date.now() + timeoutMs;
  while (Date.now() < until) {
    await new Promise((resolve) => setTimeout(resolve, 500));
    await loadStatus();
    if (mode.value !== modeName) return true;
  }
  return false;
}

async function teach() {
  busy.value = true;
  enrollResult.value = null;
  try {
    const started = await callApi(
      `/api/kws/enroll?seconds=${ENROLL_SECONDS}`,
      { method: "POST" },
      "Failed to start recording"
    );
    if (!started) return;
    await loadStatus();
    await waitWhile("enrolling", (ENROLL_SECONDS + 4) * 1000);
    enrollResult.value = status.value?.enroll ?? null;
  } finally {
    busy.value = false;
  }
}

async function runTest() {
  busy.value = true;
  try {
    const started = await callApi(
      `/api/kws/test?seconds=${TEST_SECONDS}`,
      { method: "POST" },
      "Failed to start the test"
    );
    if (!started) return;
    await loadStatus();
    await waitWhile("testing", (TEST_SECONDS + 5) * 1000);
  } finally {
    busy.value = false;
  }
}

async function forget() {
  busy.value = true;
  try {
    await callApi("/api/kws/templates", { method: "DELETE" }, "Failed to forget the word");
    enrollResult.value = null;
    await loadStatus();
  } finally {
    busy.value = false;
  }
}

async function ringNow() {
  const started = await callApi("/api/alarm/test", { method: "POST" }, "Failed to ring the alarm");
  if (started) {
    lastStop.value = "";
    alarmRinging.value = true;
  }
}

async function stopRinging() {
  await callApi("/api/alarm/test", { method: "DELETE" }, "Failed to stop the alarm");
}

const enrollText = computed(() => {
  const r = enrollResult.value;
  if (!r) return null;
  if (r.status === 0) return { type: "success", text: "Word added." };
  if (r.status === -1)
    return { type: "warning", text: "Nothing was heard - speak clearly, close to the frame." };
  if (r.status === -2)
    return { type: "warning", text: "That was too long - use one short word (under 1.5 s)." };
  if (r.status === -4)
    return {
      type: "warning",
      text: "That does not sound like the earlier examples. Say the same word the same way - or press Forget to start over with a new word.",
    };
  return { type: "error", text: "The recording failed." };
});

const testText = computed(() => {
  const t = status.value?.test;
  if (!t || (t.utterances === 0 && t.detections === 0)) return null;
  return `Heard ${t.utterances} utterance(s), the stop word ${t.detections} time(s)${
    t.best_score != null
      ? ` (best distance ${t.best_score}, threshold ${status.value.threshold})`
      : ""
  }.`;
});

onUnmounted(stopPolling);
</script>

<template>
  <div>
    <template v-if="props.voiceAvailable">
      <div class="text-subtitle-1 mb-2">Stop by voice</div>
      <div class="text-caption text-medium-emphasis mb-3">
        Teach the frame a short stop word (for example "Stop"). While the alarm rings, the frame
        listens in the pauses between the notes and stops when it hears the word. It recognises your
        voice and your pronunciation only - nothing is recorded or sent anywhere. The KEY button
        keeps working. Teach the word {{ maxTemplates }} times in slightly different ways for the
        best result.
      </div>

      <v-switch
        :model-value="alarmStop"
        color="primary"
        density="compact"
        hide-details
        :disabled="templates === 0"
        :loading="savingSwitch"
        label="Stop the ringing alarm with the stop word"
        @update:model-value="setAlarmStop"
      />
      <div v-if="templates === 0" class="text-caption text-medium-emphasis mb-2">
        Teach at least one example first.
      </div>

      <div class="d-flex align-center flex-wrap ga-2 mt-3">
        <v-btn
          variant="outlined"
          :loading="busy && mode === 'enrolling'"
          :disabled="busy || alarmRinging || templates >= maxTemplates"
          @click="teach"
        >
          <v-icon start>mdi-microphone-plus</v-icon>
          Teach {{ templates }}/{{ maxTemplates }}
        </v-btn>
        <v-btn
          variant="outlined"
          :loading="busy && mode === 'testing'"
          :disabled="busy || alarmRinging || templates === 0"
          @click="runTest"
        >
          <v-icon start>mdi-ear-hearing</v-icon>
          Test
        </v-btn>
        <v-btn variant="text" color="error" :disabled="busy || templates === 0" @click="forget">
          <v-icon start>mdi-delete</v-icon>
          Forget
        </v-btn>
      </div>
      <div v-if="mode === 'enrolling'" class="text-body-2 mt-2">
        Say the word now ({{ ENROLL_SECONDS }} s) ...
      </div>
      <v-alert
        v-if="enrollText && mode !== 'enrolling'"
        :type="enrollText.type"
        variant="tonal"
        density="compact"
        class="mt-3"
      >
        {{ enrollText.text }}
      </v-alert>
      <div v-if="testText && mode !== 'testing'" class="text-caption mt-2">{{ testText }}</div>

      <div class="text-subtitle-2 mt-4">Detection threshold</div>
      <v-switch
        :model-value="thresholdAuto"
        color="primary"
        density="compact"
        hide-details
        :disabled="templates === 0"
        :loading="savingThreshold"
        label="Automatic (from how much the taught examples differ)"
        @update:model-value="onThresholdAuto"
      />
      <v-slider
        v-model="thresholdSlider"
        :min="thresholdMin"
        :max="thresholdMax"
        :step="0.5"
        :disabled="thresholdAuto || templates === 0"
        thumb-label
        hide-details
        density="compact"
        color="error"
        class="mt-2"
        @start="draggingThreshold = true"
        @end="saveThreshold(thresholdSlider)"
      >
        <template #prepend><span class="text-caption">Strict</span></template>
        <template #append
          ><span class="text-caption">Forgiving - {{ thresholdSlider }}</span></template
        >
      </v-slider>
      <div class="text-caption text-medium-emphasis mt-1">
        Current threshold: {{ effectiveThreshold }} ({{ thresholdAuto ? "automatic" : "fixed" }}). A
        spoken word counts as the stop word when its distance to a taught example is below this
        value. Run <b>Test</b> and say your word: if its best distance is above the threshold, move
        the slider a little above that value; if other words get accepted, lower it or teach more
        examples. The automatic value is at least 4 and usually too strict for one or two examples
        of a real voice.
      </div>
      <div v-if="mode === 'testing'" class="text-body-2 mt-2">
        Listening - say the word and other words, then wait ...
      </div>

      <v-divider class="my-4" />
    </template>

    <div class="text-subtitle-1 mb-2">Try the alarm</div>
    <div class="text-caption text-medium-emphasis mb-3">
      Rings the alarm right now with the current settings (volume, ring duration{{
        props.voiceAvailable ? ", stop word" : ""
      }}).
    </div>
    <v-btn v-if="!alarmRinging" variant="outlined" :disabled="busy" @click="ringNow">
      <v-icon start>mdi-alarm-light</v-icon>
      Ring now
    </v-btn>
    <v-btn v-else color="error" variant="flat" @click="stopRinging">
      <v-icon start>mdi-alarm-off</v-icon>
      Stop
    </v-btn>
    <span v-if="alarmRinging" class="text-body-2 ml-3">Ringing ...</span>
    <div v-else-if="lastStop" class="text-caption mt-2">
      Last ring: {{ stopReasonText[lastStop] || lastStop }}.
    </div>
  </div>
</template>
