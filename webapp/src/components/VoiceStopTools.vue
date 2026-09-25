<script setup>
import { ref, computed, onUnmounted } from "vue";

const props = defineProps({
  // Speaker and microphone on this board (an Alarm Clock build): the stop word part is shown.
  voiceAvailable: { type: Boolean, default: false },
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

let pollTimer = null;

const templates = computed(() => status.value?.templates ?? 0);
const maxTemplates = computed(() => status.value?.max_templates ?? 5);
const mode = computed(() => status.value?.mode ?? "idle");
const alarmStop = computed(() => status.value?.alarm_stop === true);

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

function startPolling() {
  if (!pollTimer) pollTimer = setInterval(tick, 1000);
}

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

onUnmounted(() => {
  if (pollTimer) clearInterval(pollTimer);
});

tick();
startPolling();
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
          Teach word ({{ templates }}/{{ maxTemplates }})
        </v-btn>
        <v-btn
          variant="outlined"
          :loading="busy && mode === 'testing'"
          :disabled="busy || alarmRinging || templates === 0"
          @click="runTest"
        >
          <v-icon start>mdi-ear-hearing</v-icon>
          Test ({{ TEST_SECONDS }} s)
        </v-btn>
        <v-btn variant="text" color="error" :disabled="busy || templates === 0" @click="forget">
          <v-icon start>mdi-delete</v-icon>
          Forget word
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
