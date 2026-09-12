<script setup>
import { ref, computed, onMounted, onUnmounted } from "vue";
import { useSettingsStore, useAppStore } from "../stores";
import PaletteCalibration from "./PaletteCalibration.vue";
import GrayscaleCalibration from "./GrayscaleCalibration.vue";
import ProcessingControls from "./ProcessingControls.vue";
import RotationSchedule from "./RotationSchedule.vue";
import { isValidCron } from "../utils/cron";
import { wideEdit } from "../utils/uiPrefs";

const settingsStore = useSettingsStore();
const appStore = useAppStore();

const snackbar = ref(false);
const snackbarText = ref("");
const snackbarColor = ref("success");
function showSnackbar(text, color) {
  snackbarText.value = text;
  snackbarColor.value = color;
  snackbar.value = true;
}

const testingErrorOverlay = ref(false);
async function testErrorOverlay() {
  testingErrorOverlay.value = true;
  try {
    const response = await fetch("/api/error-overlay/test", { method: "POST" });
    const data = await response.json().catch(() => ({}));
    if (response.ok) {
      showSnackbar(data.message || "Error overlay displayed", "success");
    } else {
      showSnackbar(data.message || "Failed to display error overlay", "error");
    }
  } catch (_error) {
    showSnackbar("Failed to display error overlay", "error");
  } finally {
    testingErrorOverlay.value = false;
  }
}

// The device rejects the entire config request when any schedule rule is
// invalid, empty or over the 7-rule budget — gate saving on the same checks.
const scheduleValid = computed(() => {
  const rules = settingsStore.deviceSettings.rotateCron || [];
  return rules.length >= 1 && rules.length <= 7 && rules.every((r) => isValidCron(r));
});

// Device time state
const deviceTime = ref("");
const syncingTime = ref(false);
let deviceTimestamp = null; // Unix timestamp from device
let localTimeOffset = 0; // Offset between device time and local time
let tickInterval = null;

function updateDisplayTime() {
  if (deviceTimestamp === null) return;
  // Calculate current device time based on elapsed local time
  const elapsed = Math.floor((Date.now() - localTimeOffset) / 1000);
  const currentTimestamp = deviceTimestamp + elapsed;

  // Apply timezone offset for display
  // We shift the timestamp by the offset so that toISOString() (which is UTC)
  // displays the correct local time numbers.
  const offsetHours = settingsStore.deviceSettings.timezoneOffset || 0;
  const adjustedTimestamp = currentTimestamp + offsetHours * 3600;

  const date = new Date(adjustedTimestamp * 1000);
  // Format as YYYY-MM-DD HH:MM:SS
  deviceTime.value = date.toISOString().slice(0, 19).replace("T", " ");
}

async function parseTimezone(timezoneStr) {
  if (!timezoneStr) return;

  // Posix format: UTC[+/-]H[:MM] (e.g., UTC-8 or UTC+5:30)
  // Note: POSIX sign is inverted relative to ISO8601
  let offset = 0;
  const match = timezoneStr.match(/UTC([+-]?)(\d+)(?::(\d+))?/);
  if (match) {
    const sign = match[1] === "-" ? 1 : -1; // POSIX Inverted
    const hours = parseInt(match[2]) || 0;
    const minutes = parseInt(match[3]) || 0;
    offset = sign * (hours + minutes / 60);

    // Update store if different, to keep UI in sync
    if (settingsStore.deviceSettings.timezoneOffset !== offset) {
      settingsStore.deviceSettings.timezoneOffset = offset;
    }
  }
}

async function fetchDeviceTime() {
  try {
    const response = await fetch("/api/time");
    if (response.ok) {
      const data = await response.json();
      deviceTimestamp = data.timestamp;
      localTimeOffset = Date.now();
      await parseTimezone(data.timezone);
      updateDisplayTime();
    }
  } catch (error) {
    console.error("Failed to fetch device time:", error);
  }
}

async function syncTime() {
  syncingTime.value = true;
  try {
    const response = await fetch("/api/time/sync", { method: "POST" });
    if (response.ok) {
      const data = await response.json();
      if (data.status === "success") {
        deviceTimestamp = data.timestamp;
        localTimeOffset = Date.now();
        await parseTimezone(data.timezone);
        updateDisplayTime();
      }
    }
  } catch (error) {
    console.error("Failed to sync time:", error);
  } finally {
    syncingTime.value = false;
  }
}

onMounted(() => {
  fetchDeviceTime();
  // Tick every second to update display
  tickInterval = setInterval(updateDisplayTime, 1000);
  loadDisplayHistoryCount();
});

onUnmounted(() => {
  if (tickInterval) {
    clearInterval(tickInterval);
  }
});

const tab = computed({
  get: () => settingsStore.activeSettingsTab,
  set: (val) => (settingsStore.activeSettingsTab = val),
});

const orientationOptions = computed(() => {
  const width = appStore.systemInfo.width || 800;
  const height = appStore.systemInfo.height || 480;
  const maxDim = Math.max(width, height);
  const minDim = Math.min(width, height);

  return [
    { title: `Landscape (${maxDim}×${minDim})`, value: "landscape" },
    { title: `Portrait (${minDim}×${maxDim})`, value: "portrait" },
  ];
});

// Mirrors agenda_renderer.c's agenda_background_color() exactly - the value
// list a board can actually display depends on its BOARD_HAL_DISPLAY_TYPE
// ("gc..." = grayscale, otherwise Spectra6 6-color), so this can't be one
// static list. An element/text color that happens to collide with whatever
// is picked here is automatically swapped to a safe fallback on-device
// (agenda_avoid_bg_collision()) - no need to warn about that in this UI.
const agendaBgOptions = computed(() => {
  const displayType = appStore.systemInfo.display_type || "";
  if (displayType.startsWith("gc")) {
    return [
      { title: "White", value: "white" },
      { title: "Light gray", value: "gray75" },
      { title: "Mid gray", value: "gray50" },
      { title: "Dark gray", value: "gray25" },
      { title: "Black", value: "black" },
    ];
  }
  return [
    { title: "White", value: "white" },
    { title: "Black", value: "black" },
    { title: "Yellow", value: "yellow" },
    { title: "Red", value: "red" },
    { title: "Blue", value: "blue" },
    { title: "Green", value: "green" },
  ];
});

// Per-role color pickers (agendaPriAColor etc.) only make sense on a color
// panel - grayscale has no spare hue to assign, see agenda_renderer.c's
// role_hue() comment.
const agendaIsGrayscaleBoard = computed(() => {
  const displayType = appStore.systemInfo.display_type || "";
  return displayType.startsWith("gc");
});

// Mirrors role_hue() in agenda_renderer.c exactly - only these 4 chromatic
// Spectra6 hues are ever offered, never a free color, since anything
// off-palette dithers into visual noise on real hardware (see
// priority_color()'s comment there for the full story).
const agendaHueOptions = [
  { title: "Yellow", value: "yellow" },
  { title: "Red", value: "red" },
  { title: "Blue", value: "blue" },
  { title: "Green", value: "green" },
];

// Drives the two v-for color-picker grids below (same "field list + v-for"
// shape PaletteCalibration.vue already uses for its own per-color inputs) -
// one array entry per settingsStore.deviceSettings key, instead of a
// hand-written <v-select> block per role.
const agendaTodoColorFields = [
  { key: "agendaPriAColor", label: "Priority (A)" },
  { key: "agendaPriBColor", label: "Priority (B)" },
  { key: "agendaPriCColor", label: "Priority (C)" },
  { key: "agendaPriDColor", label: "Priority (D)" },
  { key: "agendaDueOverdueColor", label: "Overdue" },
  { key: "agendaDueTodayColor", label: "Due today" },
  { key: "agendaDueLaterColor", label: "Due later" },
  { key: "agendaProjectColor", label: "+Project" },
  { key: "agendaContextColor", label: "@Context" },
];
const agendaCalendarColorFields = [
  { key: "agendaCalAColor", label: "Calendar A" },
  { key: "agendaCalBColor", label: "Calendar B" },
];

// 90/270 would swap the panel's logical dimensions, which the streaming
// pipeline and dimensionless .epdgz payloads can't represent; portrait
// mounting is handled by the orientation setting instead
const rotationOptions = [
  { title: "0°", value: 0 },
  { title: "180°", value: 180 },
];

const rotationModeOptions = computed(() => {
  const options = [
    { title: "URL - Fetch image from URL", value: "url" },
    { title: "Telegram - Receive images via Telegram bot", value: "telegram" },
  ];
  if (appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage) {
    options.unshift({ title: "Storage - Rotate through images", value: "storage" });
  }
  return options;
});

const sdRotationModeOptions = [
  { title: "Random - Shuffle images", value: "random" },
  { title: "Sequential - In sequence", value: "sequential" },
];

const saving = ref(false);
const saveSuccess = ref(false);

function onPresetChange(preset) {
  if (preset !== "custom") {
    settingsStore.applyPreset(preset);
  }
}

function onParamsUpdate(newParams) {
  Object.assign(settingsStore.params, newParams);
}

const saveMessage = ref("");
const saveError = ref(false);

const showFactoryResetDialog = ref(false);
const resetting = ref(false);
const showImportDialog = ref(false);
const importData = ref(null);
const importFileName = ref("");

const displayHistoryCount = ref(null);
const confirmingHistoryReset = ref(false);
const resettingHistory = ref(false);

async function loadDisplayHistoryCount() {
  try {
    const response = await fetch("/api/history");
    if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
      return;
    }
    const data = await response.json();
    displayHistoryCount.value = data.count ?? null;
  } catch (_error) {
    console.log("Display history API not available (standalone mode)");
  }
}

async function resetDisplayHistory() {
  resettingHistory.value = true;
  try {
    await fetch("/api/history", { method: "DELETE" });
    await loadDisplayHistoryCount();
  } catch (_error) {
    console.log("Failed to reset display history");
  } finally {
    resettingHistory.value = false;
    confirmingHistoryReset.value = false;
  }
}

// Default OFF: an export is downloaded to disk as plaintext JSON, so
// credentials should only end up in it when the user explicitly opts in
// (e.g. to get a fully self-contained backup for re-import elsewhere).
// Note this can only cover fields GET /api/config actually returns -
// wifi_password/agenda_todo_url/agenda_cal_url/agenda_cal_url2 are
// deliberately write-only at the device level (never in the GET response
// at all), so no frontend checkbox can include them; those must always be
// re-entered by hand after an import.
const exportIncludeSecrets = ref(false);

async function exportConfig() {
  try {
    const [configRes, processingRes, paletteRes, albumsRes] = await Promise.all([
      fetch("/api/config"),
      fetch("/api/settings/processing"),
      fetch("/api/settings/palette"),
      fetch("/api/albums"),
    ]);

    const exported = {};

    if (configRes.ok) {
      const config = await configRes.json();
      // Always write-only at the device level - never returned by GET, so
      // these deletes are belt-and-suspenders and unaffected by the
      // checkbox below.
      delete config.wifi_password;
      delete config.agenda_todo_url;
      delete config.agenda_cal_url;
      delete config.agenda_cal_url2;
      // These 5 ARE returned by GET /api/config in plaintext - only strip
      // them when the user hasn't opted in to a full-credentials export.
      if (!exportIncludeSecrets.value) {
        delete config.access_token;
        delete config.http_header_value;
        delete config.telegram_bot_token;
        delete config.openai_api_key;
        delete config.google_api_key;
      }
      exported.config = config;
    }
    if (processingRes.ok) exported.processing = await processingRes.json();
    if (paletteRes.ok) exported.palette = await paletteRes.json();
    if (albumsRes.ok && albumsRes.headers.get("content-type")?.includes("application/json")) {
      const albums = await albumsRes.json();
      // Per-album enable/disable toggle - the rest (name, image_count) is
      // content, not a setting, and wouldn't make sense to "import" onto a
      // different device's storage anyway.
      exported.albums = albums.map((a) => ({ name: a.name, enabled: a.enabled }));
    }

    const blob = new Blob([JSON.stringify(exported, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    const deviceName = settingsStore.deviceSettings.deviceName || "photoframe";
    a.download = `${deviceName.toLowerCase().replace(/\s+/g, "-")}-config.json`;
    a.click();
    URL.revokeObjectURL(url);
  } catch (error) {
    console.error("Failed to export config:", error);
  }
}

const downloadingLog = ref(false);

async function downloadDebugLog() {
  downloadingLog.value = true;
  try {
    const response = await fetch("/api/debug/log");
    if (!response.ok) {
      saveError.value = true;
      saveMessage.value = "No debug logs available";
      setTimeout(() => (saveError.value = false), 5000);
      return;
    }
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    const deviceName = settingsStore.deviceSettings.deviceName || "photoframe";
    a.download = `${deviceName.toLowerCase().replace(/\s+/g, "-")}-debug.log`;
    a.click();
    URL.revokeObjectURL(url);
  } catch (error) {
    console.error("Failed to download debug log:", error);
    saveError.value = true;
    saveMessage.value = "Failed to download debug logs";
    setTimeout(() => (saveError.value = false), 5000);
  } finally {
    downloadingLog.value = false;
  }
}

const clearingLog = ref(false);

async function clearDebugLog() {
  clearingLog.value = true;
  try {
    const response = await fetch("/api/debug/log", { method: "DELETE" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    saveSuccess.value = true;
    saveMessage.value = "Debug logs cleared";
    setTimeout(() => (saveSuccess.value = false), 3000);
  } catch (error) {
    console.error("Failed to clear debug logs:", error);
    saveError.value = true;
    saveMessage.value = "Failed to clear debug logs";
    setTimeout(() => (saveError.value = false), 5000);
  } finally {
    clearingLog.value = false;
  }
}

const organizingCropVariants = ref(false);

async function organizeCropVariants() {
  organizingCropVariants.value = true;
  try {
    const response = await fetch("/api/albums/organize-crop", { method: "POST" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const data = await response.json();
    saveSuccess.value = true;
    saveMessage.value =
      data.moved > 0
        ? `Moved ${data.moved} file(s) into crop/ folders`
        : "Every album's crop/ folder is already up to date";
    setTimeout(() => (saveSuccess.value = false), 3000);
  } catch (error) {
    console.error("Failed to organize crop/ folders:", error);
    saveError.value = true;
    saveMessage.value = "Failed to organize crop/ folders";
    setTimeout(() => (saveError.value = false), 5000);
  } finally {
    organizingCropVariants.value = false;
  }
}

function onImportFileSelected(event) {
  const file = event.target.files?.[0];
  if (!file) return;

  importFileName.value = file.name;
  const reader = new FileReader();
  reader.onload = (e) => {
    try {
      importData.value = JSON.parse(e.target.result);
      showImportDialog.value = true;
    } catch {
      saveError.value = true;
      saveMessage.value = "Invalid JSON file";
      setTimeout(() => (saveError.value = false), 5000);
    }
  };
  reader.readAsText(file);
  // Reset input so the same file can be selected again
  event.target.value = "";
}

async function performImport() {
  if (!importData.value) return;

  showImportDialog.value = false;
  saving.value = true;

  try {
    const promises = [];

    if (importData.value.config) {
      promises.push(
        fetch("/api/config", {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.config),
        })
      );
    }
    if (importData.value.processing) {
      promises.push(
        fetch("/api/settings/processing", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.processing),
        })
      );
    }
    if (importData.value.palette) {
      promises.push(
        fetch("/api/settings/palette", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.palette),
        })
      );
    }
    if (Array.isArray(importData.value.albums)) {
      for (const album of importData.value.albums) {
        if (album && typeof album.name === "string" && typeof album.enabled === "boolean") {
          promises.push(
            fetch(`/api/albums/enabled?name=${encodeURIComponent(album.name)}`, {
              method: "PUT",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ enabled: album.enabled }),
            })
          );
        }
      }
    }

    await Promise.all(promises);

    // Reload all settings from device
    await Promise.all([
      settingsStore.loadDeviceSettings(),
      settingsStore.loadSettings(),
      settingsStore.loadPalette(),
      appStore.loadAlbums(),
    ]);

    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = "Config imported successfully!";
    setTimeout(() => (saveSuccess.value = false), 3000);
  } catch (error) {
    console.error("Failed to import config:", error);
    saveError.value = true;
    saveMessage.value = "Failed to import config";
    setTimeout(() => (saveError.value = false), 5000);
  } finally {
    saving.value = false;
    importData.value = null;
  }
}

async function saveSettings() {
  saving.value = true;

  // Save both device settings and processing settings
  const [deviceResult, processingSuccess] = await Promise.all([
    settingsStore.saveDeviceSettings(),
    settingsStore.saveSettings(),
  ]);

  saving.value = false;

  if (deviceResult.success && processingSuccess) {
    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = deviceResult.message || "Settings saved!";
    setTimeout(() => (saveSuccess.value = false), 3000);

    // Refresh device time in case timezone changed
    await fetchDeviceTime();
  } else {
    // Show error message
    saveError.value = true;
    saveSuccess.value = false;
    saveMessage.value = deviceResult.message || "Failed to save settings";
    setTimeout(() => (saveError.value = false), 5000);
  }
}

async function performFactoryReset() {
  resetting.value = true;
  const result = await settingsStore.factoryReset();
  resetting.value = false;
  showFactoryResetDialog.value = false;

  if (result.success) {
    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = result.message;
    setTimeout(() => (saveSuccess.value = false), 3000);
  } else {
    saveError.value = true;
    saveSuccess.value = false;
    saveMessage.value = result.message;
    setTimeout(() => (saveError.value = false), 5000);
  }
}
</script>

<template>
  <div>
    <v-card style="overflow: visible">
      <v-card-title class="d-flex align-center">
        <v-icon icon="mdi-cog" class="mr-2" />
        Settings
      </v-card-title>

      <v-tabs v-model="tab" color="primary" show-arrows density="compact">
        <v-tab value="general"> General </v-tab>
        <v-tab value="autoRotate"> Auto Rotate </v-tab>
        <v-tab value="agenda"> Agenda </v-tab>
        <v-tab value="power"> Power </v-tab>
        <v-tab value="homeAssistant"> Home Assistant </v-tab>
        <v-tab value="processing"> Processing </v-tab>
        <v-tab value="ai"> AI Generation </v-tab>
        <v-tab value="calibration">
          {{ appStore.isGrayscale ? "Grayscale" : "Palette" }}
        </v-tab>
        <v-tab value="maintenance"> Maintenance </v-tab>
      </v-tabs>

      <v-card-text>
        <v-tabs-window v-model="tab">
          <!-- General Tab -->
          <v-tabs-window-item value="general">
            <v-row class="mt-2">
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.deviceName"
                  label="Device Name"
                  variant="outlined"
                  hint="Used for mDNS hostname (e.g., 'Living Room Frame' → living-room-frame.local)"
                  persistent-hint
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.wifiSsid"
                  label="WiFi SSID"
                  variant="outlined"
                  hint="Network name to connect to"
                  persistent-hint
                />
              </v-col>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.wifiPassword"
                  label="WiFi Password"
                  type="password"
                  variant="outlined"
                  hint="Leave empty to keep current password"
                  persistent-hint
                  placeholder="••••••••"
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-select
                  v-model="settingsStore.deviceSettings.displayOrientation"
                  :items="orientationOptions"
                  item-title="title"
                  item-value="value"
                  label="Display Orientation"
                  variant="outlined"
                />
              </v-col>
              <v-col cols="12" md="6">
                <v-select
                  v-model="settingsStore.deviceSettings.displayRotationDeg"
                  :items="rotationOptions"
                  item-title="title"
                  item-value="value"
                  label="Display Rotation (deg)"
                  variant="outlined"
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-text-field
                  :model-value="deviceTime || 'Loading...'"
                  label="Device Time"
                  variant="outlined"
                  readonly
                  hint="Click sync to update from NTP server"
                  persistent-hint
                >
                  <template #append-inner>
                    <v-btn
                      icon
                      variant="text"
                      size="small"
                      :loading="syncingTime"
                      @click="syncTime"
                    >
                      <v-icon>mdi-sync</v-icon>
                      <v-tooltip activator="parent" location="top">Sync NTP</v-tooltip>
                    </v-btn>
                  </template>
                </v-text-field>
              </v-col>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model.number="settingsStore.deviceSettings.timezoneOffset"
                  label="Timezone (UTC offset)"
                  type="number"
                  :min="-12"
                  :max="14"
                  :step="0.5"
                  variant="outlined"
                  hint="e.g., -8 for PST, +1 for CET, +8 for CST"
                  persistent-hint
                />
              </v-col>
            </v-row>
            <!-- Advanced network settings (#43): collapsed by default — NTP,
                 static IP and DNS override are tinkerer territory. -->
            <v-expansion-panels class="mt-2" variant="accordion">
              <v-expansion-panel title="Advanced network settings" elevation="0">
                <v-expansion-panel-text>
                  <v-row>
                    <v-col cols="12" md="6">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.ntpServer"
                        label="NTP Server"
                        variant="outlined"
                        hint="e.g., pool.ntp.org, cn.pool.ntp.org, or a local IP"
                        persistent-hint
                      />
                    </v-col>
                    <v-col cols="12" md="6">
                      <v-select
                        v-model="settingsStore.deviceSettings.ipMode"
                        :items="[
                          { title: 'Automatic (DHCP)', value: 'dhcp' },
                          { title: 'Static IP', value: 'static' },
                        ]"
                        label="IP Configuration"
                        variant="outlined"
                        hint="Applied on the next boot / wake"
                        persistent-hint
                      />
                    </v-col>
                  </v-row>
                  <v-row v-if="settingsStore.deviceSettings.ipMode === 'static'">
                    <v-col cols="12" md="4">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.staticIp"
                        label="IP Address"
                        variant="outlined"
                        placeholder="192.168.1.50"
                      />
                    </v-col>
                    <v-col cols="12" md="4">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.staticNetmask"
                        label="Netmask"
                        variant="outlined"
                      />
                    </v-col>
                    <v-col cols="12" md="4">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.staticGateway"
                        label="Gateway"
                        variant="outlined"
                        placeholder="192.168.1.1"
                      />
                    </v-col>
                  </v-row>
                  <v-row>
                    <v-col cols="12" md="6">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.dnsServer"
                        label="DNS Server"
                        variant="outlined"
                        :hint="
                          settingsStore.deviceSettings.ipMode === 'static'
                            ? 'Leave empty to use the gateway'
                            : 'Optional override; leave empty to use DHCP-provided DNS'
                        "
                        persistent-hint
                      />
                    </v-col>
                  </v-row>
                </v-expansion-panel-text>
              </v-expansion-panel>
            </v-expansion-panels>
          </v-tabs-window-item>

          <!-- Auto Rotate Tab -->
          <v-tabs-window-item value="autoRotate">
            <v-switch
              v-model="settingsStore.deviceSettings.autoRotate"
              label="Enable Auto-Rotate"
              color="primary"
              class="mb-2"
              hide-details
            />

            <div class="ml-10">
              <RotationSchedule
                v-model="settingsStore.deviceSettings.rotateCron"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />

              <v-select
                v-model="settingsStore.deviceSettings.rotationMode"
                :items="rotationModeOptions"
                item-title="title"
                item-value="value"
                label="Rotation Mode"
                variant="outlined"
                class="mt-8 mb-4"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />

              <div class="d-flex align-center flex-wrap ga-3 mb-4">
                <span class="text-caption text-medium-emphasis">
                  <template v-if="displayHistoryCount !== null">
                    {{ displayHistoryCount }} image{{ displayHistoryCount === 1 ? "" : "s" }} shown
                    this cycle
                  </template>
                  <template v-else>Display history unavailable</template>
                </span>
                <v-btn
                  v-if="displayHistoryCount"
                  variant="outlined"
                  size="small"
                  color="error"
                  @click="confirmingHistoryReset = true"
                >
                  <v-icon icon="mdi-history" start />
                  Reset History
                </v-btn>
              </div>
              <div class="text-caption text-medium-emphasis mb-4">
                Random rotation (and the Telegram-mode fallback) tracks which images have already
                been shown so it can cycle through every one once before repeating - this is that
                count. Resetting starts a fresh cycle immediately. Also available via the
                "/clear_history" Telegram bot command.
              </div>

              <v-expand-transition>
                <!-- Not restricted to rotation_mode "storage": every rotation mode can end
                     up calling display_manager_rotate_from_storage() (Telegram/URL-mode
                     fallback reuse the exact same random/sequential-pick + pairing logic
                     as the primary Storage mode), so these settings matter regardless of
                     which mode is currently active. -->
                <v-card v-if="settingsStore.deviceSettings.autoRotate" variant="tonal" class="mb-4">
                  <v-card-text>
                    <v-select
                      v-model="settingsStore.deviceSettings.sdRotationMode"
                      :items="sdRotationModeOptions"
                      item-title="title"
                      item-value="value"
                      label="Storage Rotation Logic"
                      variant="outlined"
                      hide-details
                      class="mb-4"
                    />

                    <v-switch
                      v-model="settingsStore.deviceSettings.rotationPairingEnabled"
                      label="Combine mismatched-orientation images picked during rotation"
                      color="primary"
                      hide-details
                    />
                    <div class="text-caption text-medium-emphasis">
                      Not to be confused with the similarly-named "Combine mismatched-orientation
                      Telegram receives" option in the Telegram tab - that one pairs incoming photos
                      as they arrive; this one applies whenever an image gets picked from an album
                      during rotation, which also includes the fallback picture shown on a Telegram-
                      or URL-mode wake with nothing new to display. When the next image to show
                      doesn't match the panel's orientation, looks for another mismatched image in
                      the active album(s) and combines them side by side instead of showing one
                      letterboxed. The combined image is saved permanently in the album (the two
                      originals are kept too). Also togglable via the "/rotation_pairing" Telegram
                      bot command.
                    </div>
                    <v-alert
                      v-if="
                        settingsStore.deviceSettings.rotationPairingEnabled &&
                        settingsStore.deviceSettings.sdRotationMode === 'sequential'
                      "
                      type="warning"
                      variant="tonal"
                      density="compact"
                      class="mt-2"
                    >
                      Only takes effect in Random rotation logic - Sequential mode ignores this
                      setting.
                    </v-alert>

                    <v-switch
                      v-model="settingsStore.deviceSettings.variantSelectionEnabled"
                      label="Use pre-rendered Cover/Fit variants"
                      color="primary"
                      hide-details
                      class="mt-2"
                    />
                    <div class="text-caption text-medium-emphasis">
                      For albums produced by process-cli's <code>--crop-output both</code> (a
                      "&lt;name&gt;.fit.&lt;ext&gt;" next to the original,
                      "&lt;name&gt;.cover.&lt;ext&gt;" in a "crop" subfolder, plus an optional
                      "&lt;name&gt;.facecrop.json"): picks whichever file matches the Scale Mode
                      setting (Processing tab) instead of re-rendering it on the device. Renders and
                      caches the missing one on-device if needed (only for a genuine,
                      not-yet-processed original). Ordinary albums are unaffected either way.
                    </div>
                  </v-card-text>
                </v-card>
              </v-expand-transition>

              <v-expand-transition>
                <v-card
                  v-if="
                    settingsStore.deviceSettings.autoRotate &&
                    settingsStore.deviceSettings.rotationMode === 'url'
                  "
                  variant="tonal"
                  class="mb-4"
                >
                  <v-card-text>
                    <v-text-field
                      v-model="settingsStore.deviceSettings.imageUrl"
                      label="Image URL"
                      variant="outlined"
                      hide-details
                      class="mb-4"
                    />

                    <div
                      v-if="settingsStore.deviceSettings.caCertSet"
                      class="mb-4 d-flex flex-column ga-1"
                    >
                      <v-chip
                        color="success"
                        size="small"
                        variant="tonal"
                        style="align-self: flex-start"
                      >
                        <v-icon start>mdi-check-circle</v-icon>
                        Certificate Pinned
                      </v-chip>
                      <div class="text-caption text-medium-emphasis">
                        The TLS certificate for this HTTPS URL is pinned. It will re-pin
                        automatically when you change the URL.
                      </div>
                    </div>

                    <v-alert
                      v-if="settingsStore.deviceSettings.lastFetchError"
                      type="error"
                      variant="tonal"
                      density="compact"
                      class="mb-4"
                    >
                      Last fetch error: {{ settingsStore.deviceSettings.lastFetchError }}
                    </v-alert>

                    <v-checkbox
                      v-if="
                        appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage
                      "
                      v-model="settingsStore.deviceSettings.saveDownloadedImages"
                      label="Save downloaded images to Downloads album"
                      color="primary"
                      class="mb-8"
                      hide-details
                    />

                    <v-text-field
                      v-model="settingsStore.deviceSettings.accessToken"
                      label="Access Token (Optional)"
                      variant="outlined"
                      hint="Sets Authorization: Bearer header"
                      persistent-hint
                      class="mt-4"
                    />

                    <v-row class="mt-4">
                      <v-col cols="12" md="6">
                        <v-text-field
                          v-model="settingsStore.deviceSettings.httpHeaderKey"
                          label="Custom Header Name"
                          variant="outlined"
                          placeholder="e.g., X-API-Key"
                        />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-text-field
                          v-model="settingsStore.deviceSettings.httpHeaderValue"
                          label="Custom Header Value"
                          variant="outlined"
                        />
                      </v-col>
                    </v-row>
                  </v-card-text>
                </v-card>
              </v-expand-transition>

              <v-expand-transition>
                <v-card
                  v-if="
                    settingsStore.deviceSettings.autoRotate &&
                    settingsStore.deviceSettings.rotationMode === 'telegram'
                  "
                  variant="tonal"
                  class="mb-4"
                >
                  <v-card-text>
                    <v-chip
                      :color="
                        settingsStore.deviceSettings.telegramConfigured ? 'success' : 'warning'
                      "
                      size="small"
                      variant="tonal"
                      class="mb-4"
                    >
                      <v-icon start>{{
                        settingsStore.deviceSettings.telegramConfigured
                          ? "mdi-check-circle"
                          : "mdi-alert-circle-outline"
                      }}</v-icon>
                      {{
                        settingsStore.deviceSettings.telegramConfigured
                          ? "Telegram bot configured"
                          : "Bot token and chat ID required"
                      }}
                    </v-chip>

                    <v-text-field
                      v-model="settingsStore.deviceSettings.telegramBotToken"
                      label="Telegram Bot Token"
                      variant="outlined"
                      hint="From @BotFather, e.g. 123456789:AAbecomes..."
                      persistent-hint
                      class="mb-4"
                    />

                    <v-text-field
                      v-model="settingsStore.deviceSettings.telegramChatId"
                      label="Telegram Chat ID"
                      variant="outlined"
                      hint="Only messages from this numeric chat/group ID are processed"
                      persistent-hint
                      class="mb-4"
                    />

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramPairingEnabled"
                      label="Combine mismatched-orientation Telegram receives"
                      color="primary"
                      class="mb-2"
                      hide-details
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      Not to be confused with the similarly-named "Combine mismatched-orientation
                      images picked during rotation" option in the Auto Rotate tab - that one
                      applies to album picks during rotation; this one pairs incoming Telegram
                      photos as they arrive. Two portrait photos on a landscape frame (or two
                      landscape photos on a portrait frame) are combined side by side / stacked. A
                      lone mismatched photo is held back until its partner arrives. Also togglable
                      via the "/pairing" bot command.
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramWakeNotifyEnabled"
                      label="Send a status ping on every wake"
                      color="primary"
                      class="mb-2"
                      hide-details
                      :disabled="settingsStore.deviceSettings.telegramPowerSaveEnabled"
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      Sends SSID, IP, battery, reset/wake reason and rotation schedule to the bot on
                      every poll, even when there are no new messages. Also togglable via the
                      "/wake_notify" bot command. Suppressed while Power save mode is on (this
                      setting is remembered, not cleared, and resumes if it's turned back off).
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramFallbackRotationEnabled"
                      label="Change display on a wake with no new photo"
                      color="primary"
                      class="mb-2"
                      hide-details
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      On (default): a wake with no new Telegram image still falls back to normal
                      album rotation, same as the other rotation modes. Off: the display only
                      changes on a wake that actually receives a new Telegram photo - every other
                      wake (timer/button) leaves the current image untouched. Also togglable via the
                      "/fallback_rotation" bot command.
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramFallbackOnErrorEnabled"
                      label="Still fall back to album rotation if the Telegram connection fails"
                      color="primary"
                      class="mb-2 ml-4"
                      hide-details
                      :disabled="settingsStore.deviceSettings.telegramFallbackRotationEnabled"
                    />
                    <div class="text-caption text-medium-emphasis mb-4 ml-4">
                      Only relevant while the option above is off. On (default): a poll that fails
                      outright (Telegram unreachable, or not configured at all) is still treated as
                      an exception and falls back to normal album rotation. Off: a failed poll also
                      leaves the display unchanged, folded into the same strict policy as "no new
                      photo". Also togglable via the "/fallback_rotation_on_error" bot command.
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramRotationNotifyEnabled"
                      label="Notify when a wake shows a non-Telegram image"
                      color="primary"
                      class="mb-2"
                      hide-details
                      :disabled="
                        !settingsStore.deviceSettings.telegramFallbackRotationEnabled ||
                        settingsStore.deviceSettings.telegramPowerSaveEnabled
                      "
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      When a wake falls back to normal album rotation (no new Telegram image that
                      cycle), sends a thumbnail of whatever got displayed instead, so the chat still
                      shows what's currently on the frame. Also togglable via the "/rotation_notify"
                      bot command. Suppressed while Power save mode is on (this setting is
                      remembered, not cleared, and resumes if it's turned back off).
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramPowerSaveEnabled"
                      label="Power save mode"
                      color="primary"
                      class="mb-2"
                      hide-details
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      Minimizes wake duration and WiFi-on time on an automatic (timer) wake: fewer
                      WiFi/Telegram retries before giving up, skips the post-rotation config-sync
                      window, and skips the per-photo "saved" confirmation reply, the wake status
                      ping, and the fallback-rotation photo notification (those three settings are
                      only grayed out, not cleared - they resume as configured if this is turned
                      back off). Never affects a manual button-triggered wake, which always keeps
                      its full retry budget and window - a deliberate escape hatch to reach this
                      page even with this on. Also togglable via the "/power_save" bot command.
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramPowerSaveLatestOnly"
                      label="Only process the newest update"
                      color="primary"
                      class="mb-2 ml-4"
                      hide-details
                      :disabled="!settingsStore.deviceSettings.telegramPowerSaveEnabled"
                    />
                    <div class="text-caption text-medium-emphasis mb-4 ml-4">
                      Only relevant while the option above is on. Processes only the single newest
                      photo/document in a poll batch and discards every other update, message, and
                      "/" command in that batch - permanently (Telegram never redelivers them). The
                      surviving image always displays alone, never combined via orientation-pairing.
                      Also togglable via the "/power_save_latest_only" bot command.
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramKeepOriginalsEnabled"
                      label="Keep original photos as received"
                      color="primary"
                      class="mb-2"
                      hide-details
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      Saves a copy of each Telegram photo exactly as received, before e-paper
                      processing (dithering/palette quantization), under Telegram/Originals on the
                      SD card. Not shown in the gallery or rotation. Also togglable via the
                      "/keep_originals" bot command.
                    </div>

                    <v-select
                      v-model="settingsStore.deviceSettings.telegramImageFormat"
                      :items="[
                        {
                          title: 'EPDGZ (recommended - smaller, faster to display)',
                          value: 'epdgz',
                        },
                        { title: 'PNG (larger, for compatibility/inspection)', value: 'png' },
                      ]"
                      label="On-device image format"
                      variant="outlined"
                      density="compact"
                      hide-details
                      class="mb-2"
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      Format used when the device itself converts a received Telegram photo for the
                      album. EPDGZ stores the already-resolved palette index, gzip-compressed - no
                      per-pixel color re-matching needed on every future display, unlike PNG.
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.telegramDedupEnabled"
                      label="Skip duplicate photos/files"
                      color="primary"
                      class="mb-2"
                      hide-details
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      Compares Telegram's own content-based file identifier before downloading, so
                      the same photo or file resent/forwarded again is skipped instead of downloaded
                      and displayed a second time. Remembers the last 30 received items across deep
                      sleep; a skipped duplicate gets a short reply instead of an error.
                    </div>

                    <v-switch
                      v-model="settingsStore.deviceSettings.showExifDatetimeEnabled"
                      label="Show capture date as caption when a photo has none"
                      color="primary"
                      class="mb-2"
                      hide-details
                    />
                    <div class="text-caption text-medium-emphasis mb-4">
                      When a photo has no caption of its own, falls back to its EXIF
                      "DateTimeOriginal" (the camera's capture date), if present - otherwise no
                      caption is shown. Applies to Telegram-received photos and to Storage/
                      Auto-Rotate album images processed by process-cli (see
                      docs/OVERLAYS.md#capture-date-caption-for-storageauto-rotate-photos) - not to
                      Web UI album uploads, which are converted entirely in the browser and don't
                      currently extract EXIF. Also togglable via the "/exif_date" bot command.
                    </div>

                    <v-alert
                      v-if="settingsStore.deviceSettings.lastFetchError"
                      type="error"
                      variant="tonal"
                      density="compact"
                      class="mb-2"
                    >
                      Last fetch error: {{ settingsStore.deviceSettings.lastFetchError }}
                    </v-alert>

                    <div class="text-caption text-medium-emphasis">
                      Send a photo or image file to the bot, or a "/" command (/status, /restart,
                      /clear). Send /telegram_reset to immediately clear a stuck message queue.
                    </div>
                  </v-card-text>
                </v-card>
              </v-expand-transition>
            </div>
          </v-tabs-window-item>

          <!-- Agenda Tab (ToDo + Calendar) -->
          <v-tabs-window-item value="agenda">
            <v-alert type="info" variant="tonal" density="compact" class="mb-4">
              Not an overlay on a photo - whenever a wake matches the schedule below, the display is
              used exclusively to show ToDo and/or Calendar content instead of a photo, then goes
              back to sleep. Normal photo auto-rotation is unaffected and keeps running on its own
              separate schedule.
            </v-alert>

            <div class="text-subtitle-2 mb-2">ToDo</div>
            <v-switch
              v-model="settingsStore.deviceSettings.agendaTodoEnabled"
              label="Show ToDo list"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-2">
              A plain todo.txt file, re-checked every agenda wake - no API key needed. An
              unchanged file is detected via a conditional request and skips re-downloading.
              Completed tasks ("x " prefix) are never shown. Treated like a password
              field (never shown back to you) since a private feed's URL can embed an access
              token, the same way a Google Calendar link can.
            </div>
            <v-text-field
              v-model="settingsStore.deviceSettings.agendaTodoUrl"
              label="todo.txt URL"
              type="password"
              variant="outlined"
              density="compact"
              hint="Leave empty to keep the current URL"
              persistent-hint
              placeholder="••••••••"
              class="mb-4"
              :disabled="!settingsStore.deviceSettings.agendaTodoEnabled"
            />

            <v-divider class="mb-4" />

            <div class="text-subtitle-2 mb-2">Calendar</div>
            <v-switch
              v-model="settingsStore.deviceSettings.agendaCalEnabled"
              label="Show upcoming events"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-2">
              An iCalendar/ICS feed - e.g. a Google Calendar "Secret address in iCal format"
              (Calendar Settings → Integrate calendar). Google's own docs warn that only you should
              know this address - treat it like a password, never share it. A second calendar is
              optional (e.g. work alongside personal) - events from both are merged into one list,
              sorted by time, and colored by origin: Calendar A is blue, Calendar B is green
              (shown as a filled background on a light agenda background, plain colored text on a
              dark one - see Appearance below).
            </div>
            <v-row dense>
              <v-col cols="12" sm="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.agendaCalUrl"
                  label="Calendar A ICS URL"
                  type="password"
                  variant="outlined"
                  density="compact"
                  hint="Leave empty to keep the current URL"
                  persistent-hint
                  placeholder="••••••••"
                  :disabled="!settingsStore.deviceSettings.agendaCalEnabled"
                />
              </v-col>
              <v-col cols="8" sm="3">
                <v-text-field
                  v-model="settingsStore.deviceSettings.agendaCalName"
                  label="Display name"
                  variant="outlined"
                  density="compact"
                  placeholder="Calendar A"
                  hint="Shown in the Calendar header instead of &quot;Calendar A&quot;"
                  persistent-hint
                  :disabled="!settingsStore.deviceSettings.agendaCalEnabled"
                />
              </v-col>
              <v-col cols="4" sm="3">
                <v-select
                  v-model="settingsStore.deviceSettings.agendaCalDays"
                  :items="[1, 2, 3]"
                  label="Days ahead"
                  variant="outlined"
                  density="compact"
                  :disabled="!settingsStore.deviceSettings.agendaCalEnabled"
                />
              </v-col>
            </v-row>
            <v-row dense>
              <v-col cols="12" sm="8">
                <v-text-field
                  v-model="settingsStore.deviceSettings.agendaCalUrl2"
                  label="Calendar B ICS URL (optional)"
                  type="password"
                  variant="outlined"
                  density="compact"
                  hint="Leave empty to keep the current URL, or to use only one calendar"
                  persistent-hint
                  placeholder="••••••••"
                  :disabled="!settingsStore.deviceSettings.agendaCalEnabled"
                />
              </v-col>
              <v-col cols="12" sm="4">
                <v-text-field
                  v-model="settingsStore.deviceSettings.agendaCalName2"
                  label="Display name"
                  variant="outlined"
                  density="compact"
                  placeholder="Calendar B"
                  hint="Shown in the Calendar header instead of &quot;Calendar B&quot;"
                  persistent-hint
                  :disabled="!settingsStore.deviceSettings.agendaCalEnabled"
                />
              </v-col>
            </v-row>
            <v-switch
              v-model="settingsStore.deviceSettings.agendaCalWeatherEnabled"
              label="Show forecast on day dividers"
              color="primary"
              class="mt-2 mb-1"
              hide-details
              :disabled="!settingsStore.deviceSettings.agendaCalEnabled"
            />
            <div class="text-caption text-medium-emphasis mb-2">
              Appends each day's forecast to its divider, e.g. "Fr 11. [18/25 cloudy]" - reuses the
              same location/provider settings as the photo Weather Overlay (Settings → Power →
              Weather + Headline Overlays), just for this independent display path. The forecast
              only covers 3 days, so if the lookahead window reaches into a 4th day (possible late
              in the evening), that day simply shows no forecast.
            </div>
            <v-switch
              v-model="settingsStore.deviceSettings.agendaCalWeatherRightAligned"
              label="Right-align forecast"
              color="primary"
              class="mb-1"
              hide-details
              :disabled="
                !settingsStore.deviceSettings.agendaCalEnabled ||
                !settingsStore.deviceSettings.agendaCalWeatherEnabled
              "
            />
            <div class="text-caption text-medium-emphasis mb-2">
              Off (default): forecast centered on the divider line. On: forecast flush against the
              right edge instead - just a placement preference, doesn't change how much of it fits
              (works the same in both the stacked and side-by-side layout).
            </div>
            <v-switch
              v-model="settingsStore.deviceSettings.agendaCalCompactMultiday"
              label="Compact multi-day events"
              color="primary"
              class="mb-1"
              hide-details
              :disabled="!settingsStore.deviceSettings.agendaCalEnabled"
            />
            <div class="text-caption text-medium-emphasis mb-2">
              Shows a multi-day event only once, on the first visible day, with an "N/M:" prefix
              (which day of the event's full span, out of how many) instead of repeating it under
              every day it spans - e.g. an 8-day trip whose 4th day is the first one visible shows
              "4/8: Trip" that one time only.
            </div>

            <v-divider class="mb-4 mt-2" />

            <div class="text-subtitle-2 mb-2">Schedule</div>
            <div class="text-caption text-medium-emphasis mb-2">
              Independent from the Auto-Rotate schedule above - only applies while ToDo and/or
              Calendar is enabled.
            </div>
            <RotationSchedule
              v-model="settingsStore.deviceSettings.agendaCron"
              :disabled="
                !(
                  settingsStore.deviceSettings.agendaTodoEnabled ||
                  settingsStore.deviceSettings.agendaCalEnabled
                )
              "
            />

            <v-divider class="mb-4 mt-2" />

            <div class="text-subtitle-2 mb-2">Appearance</div>
            <div class="text-caption text-medium-emphasis mb-2">
              Layout only matters when both ToDo and Calendar are shown together - portrait boards
              always stack them regardless of this setting (a side-by-side split would make each
              column too narrow there).
            </div>
            <v-radio-group
              v-model="settingsStore.deviceSettings.agendaStackLayout"
              inline
              density="compact"
              hide-details
              class="mb-4"
            >
              <v-radio label="Stacked (ToDo above Calendar)" :value="true" />
              <v-radio label="Side by side" :value="false" />
            </v-radio-group>
            <v-select
              v-model="settingsStore.deviceSettings.agendaBgColor"
              :items="agendaBgOptions"
              label="Background color"
              variant="outlined"
              density="compact"
              hint="Shared by both columns. If an element's own color happens to match this background, it's automatically swapped for a safe fallback."
              persistent-hint
              style="max-width: 320px"
            />

            <template v-if="!agendaIsGrayscaleBoard">
              <v-divider class="mb-4 mt-4" />
              <div class="text-subtitle-2 mb-2">Colors</div>
              <div class="text-caption text-medium-emphasis mb-3">
                Color panels only - grayscale boards have no spare hue to assign here. Any color
                that happens to match the background above is automatically swapped for a safe
                fallback, so nothing can silently disappear.
              </div>
              <div class="text-caption text-medium-emphasis mb-1">ToDo</div>
              <v-row dense>
                <v-col
                  v-for="field in agendaTodoColorFields"
                  :key="field.key"
                  cols="6"
                  sm="4"
                  md="3"
                >
                  <v-select
                    v-model="settingsStore.deviceSettings[field.key]"
                    :items="agendaHueOptions"
                    :label="field.label"
                    variant="outlined"
                    density="compact"
                    hide-details
                  />
                </v-col>
              </v-row>
              <div class="text-caption text-medium-emphasis mb-1 mt-3">Calendar</div>
              <v-row dense>
                <v-col
                  v-for="field in agendaCalendarColorFields"
                  :key="field.key"
                  cols="6"
                  sm="4"
                  md="3"
                >
                  <v-select
                    v-model="settingsStore.deviceSettings[field.key]"
                    :items="agendaHueOptions"
                    :label="field.label"
                    variant="outlined"
                    density="compact"
                    hide-details
                  />
                </v-col>
              </v-row>
            </template>
          </v-tabs-window-item>

          <!-- Power Tab -->
          <v-tabs-window-item value="power">
            <v-switch
              v-model="settingsStore.deviceSettings.deepSleepEnabled"
              label="Enable Deep Sleep"
              color="primary"
              class="mb-4"
            />

            <v-expand-transition>
              <v-alert
                v-if="!settingsStore.deviceSettings.deepSleepEnabled"
                type="warning"
                variant="tonal"
              >
                <strong>Power Consumption Notice</strong><br />
                Disabling deep sleep keeps the HTTP server accessible but significantly increases
                power consumption. Only disable if permanently powered via USB.
              </v-alert>
            </v-expand-transition>

            <v-switch
              v-model="settingsStore.deviceSettings.otaCheckEnabled"
              label="Enable automatic update checks"
              color="primary"
              class="mb-2 mt-4"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-4">
              Checks for a new firmware release once a day and on every cold boot. A manually
              triggered "Check for updates" (below) always works regardless of this setting. Useful
              to turn off for self-built/dev firmware, which otherwise always reports an "update
              available".
            </div>

            <v-switch
              v-model="settingsStore.deviceSettings.wifiPerformanceModeEnabled"
              label="Enable WiFi performance mode"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-4">
              When on (default), the frame automatically switches to full WiFi receive power (~60-70
              mA extra draw, but a much snappier web UI) whenever someone might be looking - an
              interactive wake or USB power - and drops back to WiFi power-save otherwise. Turn off
              to always stay in power-save, even during interactive use, trading web UI
              responsiveness for lower battery draw.
            </div>

            <v-switch
              v-model="settingsStore.deviceSettings.wifiTxPowerCapEnabled"
              label="Cap WiFi TX power while a battery is present"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-4">
              On (default). Associating with an AP draws a brief high-current TX burst that a
              marginal battery/PMIC rail (e.g. the PhotoPainter's original AXP2101) may not sustain
              cleanly - capping TX power lowers that peak, at some cost to WiFi range. Turn off if
              you'd rather keep full range and haven't seen any instability.
            </div>

            <v-switch
              v-model="settingsStore.deviceSettings.errorOverlayEnabled"
              label="Show error overlay on display for persistent failures"
              color="primary"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-2">
              After 3 consecutive failed WiFi connection attempts on a scheduled wake, overlays a
              short error message on the currently displayed image (without modifying the saved
              file) so the problem is visible on the frame itself, not just in logs. Also togglable
              via the "/error_overlay" Telegram bot command.
            </div>
            <v-btn
              variant="outlined"
              size="small"
              :loading="testingErrorOverlay"
              @click="testErrorOverlay"
            >
              <v-icon icon="mdi-alert-outline" start />
              Test Error Overlay
            </v-btn>
            <div class="text-caption text-medium-emphasis mt-1">
              Displays an example error message right now, regardless of the setting above - useful
              to preview what it looks like. Overlays onto the current image if there is one,
              otherwise shows it on a blank screen.
            </div>

            <v-divider class="my-6" />

            <div class="text-subtitle-2 mb-2">Weather + Headline Overlays</div>
            <div class="text-caption text-medium-emphasis mb-4">
              On-device alternative to the companion image server's weather overlay - no separate
              server required. Drawn as a text bar across the top of the image whenever a wake
              rotates to a new photo, so data is only as fresh as your rotation schedule (a sparse
              schedule means correspondingly stale weather/headlines). Only applies to Storage and
              Telegram rotation modes - URL rotation streams pixels straight to the display and has
              no image file to draw an overlay onto.
            </div>

            <v-row dense class="mb-2">
              <v-col cols="6" sm="4">
                <v-select
                  v-model="settingsStore.deviceSettings.overlayLanguage"
                  :items="[
                    { title: 'English', value: 'en' },
                    { title: 'Deutsch', value: 'de' },
                  ]"
                  label="Overlay language"
                  variant="outlined"
                  density="compact"
                  hide-details
                />
              </v-col>
              <v-col cols="6" sm="8" class="d-flex align-center">
                <v-switch
                  v-model="settingsStore.deviceSettings.overlayInvertColors"
                  label="Invert overlay colors (white bar, black text)"
                  color="primary"
                  hide-details
                />
              </v-col>
            </v-row>
            <div class="text-caption text-medium-emphasis mb-2">
              Applies to both overlays below. Language selects the weather condition wording and
              weekday abbreviations (English default: Mon..Sun; German: Mo..So).
            </div>
            <v-checkbox
              v-model="settingsStore.deviceSettings.captionInvertColorsEnabled"
              label="Also apply color inversion to Telegram photo captions"
              color="primary"
              density="compact"
              hide-details
              class="mb-4"
            />

            <v-switch
              v-model="settingsStore.deviceSettings.overlayEpdgzEnabled"
              label="Also overlay pre-rendered EPDGZ images"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-4">
              <strong
                >Off by default - if the overlays below don't seem to appear at all, check this
                first.</strong
              >
              Storage/Auto-Rotate albums are typically already-rendered EPDGZ files (not PNG); so
              are Telegram-received photos whenever "On-device image format" (Telegram tab) is set
              to EPDGZ, which it is by default. Either way, the overlay otherwise skips that file
              entirely (no weather/headline fetch either) unless this is on. Enabling this
              decodes/redraws/re-encodes that one file on every display - an extra step not needed
              for anyone who doesn't use these overlays at all, or whose images are already PNG. BMP
              images still aren't supported (no BMP decoder exists in the firmware).
            </div>

            <v-switch
              v-model="settingsStore.deviceSettings.weatherOverlayEnabled"
              label="Weather overlay"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-2">
              3-day forecast (today + next 2 days), e.g. "Wed sunny 16/24 | Thu partly cloudy 17/25
              | Fri rain -5/3" (min/max °C). Free, no API key. Also togglable via the "/weather" bot
              command.
            </div>
            <v-row dense class="mb-2">
              <v-col cols="12" sm="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.weatherLocationName"
                  label="Location name"
                  variant="outlined"
                  density="compact"
                  placeholder="Berlin"
                  hint="Geocoded once, then cached - or set lat/lon directly to skip that"
                  persistent-hint
                  :disabled="!settingsStore.deviceSettings.weatherOverlayEnabled"
                />
              </v-col>
              <v-col cols="6" sm="3">
                <v-text-field
                  v-model="settingsStore.deviceSettings.weatherLat"
                  label="Latitude (optional)"
                  variant="outlined"
                  density="compact"
                  placeholder="52.5200"
                  :disabled="!settingsStore.deviceSettings.weatherOverlayEnabled"
                />
              </v-col>
              <v-col cols="6" sm="3">
                <v-text-field
                  v-model="settingsStore.deviceSettings.weatherLon"
                  label="Longitude (optional)"
                  variant="outlined"
                  density="compact"
                  placeholder="13.4050"
                  :disabled="!settingsStore.deviceSettings.weatherOverlayEnabled"
                />
              </v-col>
            </v-row>
            <v-row dense class="mb-2">
              <v-col cols="12" sm="6">
                <v-select
                  v-model="settingsStore.deviceSettings.weatherProvider"
                  :items="[
                    { title: 'Open-Meteo (default)', value: 'open-meteo' },
                    { title: 'wttr.in', value: 'wttr.in' },
                    { title: 'yr.no (MET Norway)', value: 'yr.no' },
                  ]"
                  label="Weather data source"
                  variant="outlined"
                  density="compact"
                  hide-details
                  :disabled="!settingsStore.deviceSettings.weatherOverlayEnabled"
                />
              </v-col>
            </v-row>
            <div class="text-caption text-medium-emphasis mb-2">
              <a href="https://open-meteo.com/" target="_blank" rel="noopener">Open-Meteo</a> is the
              default. <a href="https://wttr.in/" target="_blank" rel="noopener">wttr.in</a> and
              <a href="https://api.met.no/" target="_blank" rel="noopener">yr.no</a> are free
              alternatives to switch to manually if Open-Meteo isn't reachable or reliable for your
              network/region - there's no automatic fallback between them, so pick one at a time.
            </div>
            <v-checkbox
              v-model="settingsStore.deviceSettings.weatherMultilineEnabled"
              label="Show as 3 lines (one per day) instead of one combined line"
              color="primary"
              density="compact"
              hide-details
              :disabled="
                !settingsStore.deviceSettings.weatherOverlayEnabled ||
                settingsStore.deviceSettings.headlinesOverlayEnabled
              "
            />
            <div class="text-caption text-medium-emphasis mb-4">
              Even abbreviated, a 3-day forecast can't reliably fit on one ~46-character line for
              every combination (long condition words, 3-digit negative temperatures) - one line per
              day always fits. Only available while the headlines overlay below is off (not enough
              room for both).
            </div>

            <v-switch
              v-model="settingsStore.deviceSettings.headlinesOverlayEnabled"
              label="Headlines overlay"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-2">
              Any RSS/Atom feed URL - no API key, no rate limit. Also togglable via the "/headlines"
              bot command.
            </div>
            <v-row dense>
              <v-col cols="12" sm="8">
                <v-text-field
                  v-model="settingsStore.deviceSettings.headlinesRssUrl"
                  label="RSS feed URL"
                  variant="outlined"
                  density="compact"
                  placeholder="https://www.tagesschau.de/xml/rss2/"
                  :disabled="!settingsStore.deviceSettings.headlinesOverlayEnabled"
                />
              </v-col>
              <v-col cols="12" sm="4">
                <v-select
                  v-model="settingsStore.deviceSettings.headlinesCount"
                  :items="[1, 2, 3]"
                  label="Headlines shown"
                  variant="outlined"
                  density="compact"
                  :disabled="!settingsStore.deviceSettings.headlinesOverlayEnabled"
                />
              </v-col>
            </v-row>
            <v-expand-transition>
              <v-row
                v-if="
                  settingsStore.deviceSettings.headlinesOverlayEnabled &&
                  settingsStore.deviceSettings.headlinesCount === 1
                "
                dense
              >
                <v-col cols="12" sm="6">
                  <v-select
                    v-model="settingsStore.deviceSettings.headlinesWrapLines"
                    :items="[
                      { title: 'Single line (truncated with …)', value: 1 },
                      { title: 'Wrap across 2 lines', value: 2 },
                      { title: 'Wrap across 3 lines', value: 3 },
                    ]"
                    label="Headline display"
                    variant="outlined"
                    density="compact"
                    hide-details
                  />
                </v-col>
              </v-row>
            </v-expand-transition>
            <div
              v-if="
                settingsStore.deviceSettings.headlinesOverlayEnabled &&
                settingsStore.deviceSettings.headlinesCount === 1
              "
              class="text-caption text-medium-emphasis mt-1"
            >
              Only offered with exactly 1 headline selected above - with more than one, each already
              gets its own line.
            </div>

            <v-switch
              v-model="settingsStore.deviceSettings.lowBatteryOverlayEnabled"
              label="Low battery badge"
              color="primary"
              class="mb-2 mt-4"
              hide-details
            />
            <div class="text-caption text-medium-emphasis mb-2">
              A small corner badge (not a full-width bar, unlike the overlays above) shown on every
              display update once the battery drops below the threshold below - independent of
              Telegram/Web UI reachability, so low battery is noticeable just by looking at the
              frame. Clears once the battery recovers 4 percentage points above the threshold.
            </div>
            <v-row dense>
              <v-col cols="12" sm="6">
                <v-text-field
                  v-model.number="settingsStore.deviceSettings.lowBatteryOverlayThreshold"
                  label="Show below (%)"
                  type="number"
                  :min="1"
                  :max="50"
                  variant="outlined"
                  density="compact"
                  :disabled="!settingsStore.deviceSettings.lowBatteryOverlayEnabled"
                />
              </v-col>
            </v-row>
          </v-tabs-window-item>

          <!-- Home Assistant Tab -->
          <v-tabs-window-item class="mt-2" value="homeAssistant">
            <v-switch
              v-model="settingsStore.deviceSettings.haEnabled"
              label="Enable Home Assistant integration"
              color="primary"
              class="mb-4"
              hide-details
            />
            <v-text-field
              v-model="settingsStore.deviceSettings.haUrl"
              label="Home Assistant URL"
              variant="outlined"
              placeholder="http://homeassistant.local:8123"
              hint="Configure for dynamic image serving and battery level reporting"
              persistent-hint
              :disabled="!settingsStore.deviceSettings.haEnabled"
            />
          </v-tabs-window-item>

          <!-- Processing Tab -->
          <v-tabs-window-item value="processing">
            <div class="pa-4">
              <v-select
                v-model="settingsStore.uploadImageFormat"
                :items="[
                  { title: 'EPDGZ (recommended - smaller, faster to display)', value: 'epdgz' },
                  { title: 'PNG (larger, for compatibility/inspection)', value: 'png' },
                ]"
                label="Web UI upload format"
                variant="outlined"
                density="compact"
                hide-details
                class="mb-2"
              />
              <div class="text-caption text-medium-emphasis mb-4">
                Format this browser encodes to before uploading a photo (Web UI uploads only - this
                is a local browser preference, not saved to the device). EPDGZ stores the
                already-resolved palette index, gzip-compressed - no per-pixel color re-matching
                needed on every future display, unlike PNG.
              </div>

              <v-alert v-if="wideEdit" type="info" variant="tonal" density="compact">
                Processing controls are shown next to the preview in wide-edit mode. Turn wide edit
                off (the split icon on the Upload card) to edit them here.
              </v-alert>
              <ProcessingControls
                v-else
                :params="settingsStore.params"
                :preset="settingsStore.preset"
                @update:params="onParamsUpdate"
                @update:preset="settingsStore.preset = $event"
                @preset-change="onPresetChange"
              />
            </div>
          </v-tabs-window-item>

          <!-- AI Generation Tab -->
          <v-tabs-window-item value="ai">
            <v-alert type="info" variant="tonal" density="compact" class="mt-2 mb-4">
              API keys are used for client-side AI image generation when uploading images.
            </v-alert>

            <v-text-field
              v-model="settingsStore.deviceSettings.aiCredentials.openaiApiKey"
              label="OpenAI API Key"
              variant="outlined"
              type="password"
              hint="sk-..."
              persistent-hint
              class="mb-2"
            />
            <div class="text-caption text-grey ml-2 mb-4">
              Get your API key at
              <a
                href="https://platform.openai.com/api-keys"
                target="_blank"
                class="text-primary text-decoration-none"
                >platform.openai.com</a
              >
            </div>

            <v-text-field
              v-model="settingsStore.deviceSettings.aiCredentials.googleApiKey"
              label="Google Gemini API Key"
              variant="outlined"
              type="password"
              class="mb-2"
            />
            <div class="text-caption text-grey ml-2 mb-4">
              Get your API key at
              <a
                href="https://aistudio.google.com/app/apikey"
                target="_blank"
                class="text-primary text-decoration-none"
                >aistudio.google.com</a
              >
            </div>
          </v-tabs-window-item>

          <!-- Calibration Tab -->
          <v-tabs-window-item value="calibration">
            <GrayscaleCalibration v-if="appStore.isGrayscale" />
            <PaletteCalibration v-else />
          </v-tabs-window-item>

          <!-- Maintenance Tab -->
          <v-tabs-window-item value="maintenance">
            <div class="text-subtitle-1 mt-2 mb-4">Config Backup</div>
            <v-row>
              <v-col cols="12">
                <v-checkbox
                  v-model="exportIncludeSecrets"
                  density="compact"
                  hide-details
                  class="mb-2"
                  label="Include credentials in export (Telegram bot token, AI API keys, access token, custom auth header)"
                />
                <div class="text-caption text-grey mb-3">
                  Off by default: an export is a plaintext JSON file. Enable this for a
                  fully self-contained backup, e.g. before restoring to a fresh device.
                  WiFi password and Calendar/ToDo URLs can never be included (the device
                  never returns them at all) - re-enter those manually after importing.
                </div>
                <v-btn variant="outlined" class="mr-2" @click="exportConfig">
                  <v-icon start>mdi-download</v-icon>
                  Export Config
                </v-btn>
                <v-btn variant="outlined" @click="$refs.importInput.click()">
                  <v-icon start>mdi-upload</v-icon>
                  Import Config
                </v-btn>
                <input
                  ref="importInput"
                  type="file"
                  accept=".json"
                  style="display: none"
                  @change="onImportFileSelected"
                />
              </v-col>
            </v-row>

            <v-divider class="my-6" />

            <div class="text-subtitle-1 mb-4">Debug Logging</div>
            <v-row>
              <v-col cols="12">
                <v-switch
                  v-model="settingsStore.deviceSettings.debugLogEnabled"
                  label="Save console logs to storage"
                  color="primary"
                  hide-details
                  class="mb-2"
                />
                <v-expand-transition>
                  <v-alert
                    v-if="settingsStore.deviceSettings.debugLogEnabled"
                    type="info"
                    variant="tonal"
                    density="compact"
                    class="mb-4"
                  >
                    Serial console output is mirrored to the SD card, keeping only the most recent
                    lines. Takes effect after saving.
                  </v-alert>
                </v-expand-transition>
                <v-btn
                  variant="outlined"
                  class="mr-2"
                  :loading="downloadingLog"
                  @click="downloadDebugLog"
                >
                  <v-icon start>mdi-download</v-icon>
                  Download Logs
                </v-btn>
                <v-btn variant="outlined" :loading="clearingLog" @click="clearDebugLog">
                  <v-icon start>mdi-delete</v-icon>
                  Clear Logs
                </v-btn>
              </v-col>
            </v-row>

            <v-divider class="my-6" />

            <div class="text-subtitle-1 mb-4">Cover/Fit Variant Folders</div>
            <v-row>
              <v-col cols="12">
                <div class="text-caption text-medium-emphasis mb-2">
                  For "Use pre-rendered Cover/Fit variants" (Auto Rotate tab): creates a "crop"
                  subfolder in every album (if missing) and moves any loose
                  "&lt;name&gt;.cover.&lt;ext&gt;" files there. Only relevant for albums produced by
                  process-cli's <code>--crop-output both</code> - safe to run any time, a no-op for
                  ordinary albums.
                </div>
                <v-btn
                  variant="outlined"
                  :loading="organizingCropVariants"
                  @click="organizeCropVariants"
                >
                  <v-icon start>mdi-folder-move</v-icon>
                  Organize Crop Folders
                </v-btn>
              </v-col>
            </v-row>

            <v-divider class="my-6" />

            <div class="text-subtitle-1 mb-4">Factory Reset</div>
            <v-row>
              <v-col cols="12">
                <v-btn color="error" variant="outlined" @click="showFactoryResetDialog = true">
                  <v-icon start>mdi-restore-alert</v-icon>
                  Factory Reset Device
                </v-btn>
              </v-col>
            </v-row>
          </v-tabs-window-item>
        </v-tabs-window>
      </v-card-text>

      <v-card-actions class="px-4 pb-4">
        <v-spacer />
        <v-fade-transition>
          <v-chip v-if="saveSuccess" color="success" variant="tonal">
            <v-icon icon="mdi-check" start />
            {{ saveMessage || "Settings saved!" }}
          </v-chip>
          <v-chip v-else-if="saveError" color="error" variant="tonal">
            <v-icon icon="mdi-alert-circle" start />
            {{ saveMessage || "Failed to save settings" }}
          </v-chip>
        </v-fade-transition>
        <v-tooltip
          text="Fix the rotation schedule first (invalid or too many rules)"
          location="top"
          :disabled="scheduleValid"
        >
          <template #activator="{ props: tooltipProps }">
            <span v-bind="tooltipProps">
              <v-btn
                color="primary"
                :loading="saving"
                :disabled="!scheduleValid"
                @click="saveSettings"
              >
                <v-icon icon="mdi-content-save" start />
                Save Settings
              </v-btn>
            </span>
          </template>
        </v-tooltip>
      </v-card-actions>
    </v-card>

    <!-- Display History Reset Confirmation Dialog -->
    <v-dialog v-model="confirmingHistoryReset" max-width="440">
      <v-card>
        <v-card-title class="text-error">
          <v-icon icon="mdi-alert" class="mr-2" />
          Reset Display History?
        </v-card-title>
        <v-card-text>
          This clears the "already shown" tracking for random rotation and the Telegram-mode
          fallback, starting a fresh no-repeat cycle immediately. This cannot be undone.
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="confirmingHistoryReset = false">Cancel</v-btn>
          <v-btn
            color="error"
            variant="flat"
            :loading="resettingHistory"
            @click="resetDisplayHistory"
          >
            Reset
          </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>

    <!-- Factory Reset Confirmation Dialog -->
    <v-dialog v-model="showFactoryResetDialog" max-width="500">
      <v-card>
        <v-card-title class="text-h5 text-error">
          <v-icon icon="mdi-alert" class="mr-2" />
          Confirm Factory Reset
        </v-card-title>
        <v-card-text>
          <v-alert type="error" variant="tonal" class="mb-4">
            <div class="text-subtitle-2 mb-2">This action is irreversible!</div>
            <div class="text-body-2">
              All device settings will be permanently erased, including:
            </div>
            <ul class="mt-2">
              <li>WiFi credentials</li>
              <li>Image processing settings</li>
              <li>Device configuration</li>
              <li>All custom settings</li>
            </ul>
          </v-alert>
          <div class="text-body-1 mb-3">
            The device will restart and return to factory defaults. Are you sure you want to
            continue?
          </div>
          <v-alert type="info" variant="tonal" density="compact">
            <div class="text-body-2">
              <strong>After reset:</strong> The device will create a WiFi access point named
              <strong>"PhotoFrame"</strong>. Connect to it from your device to restart the
              provisioning process.
            </div>
          </v-alert>
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showFactoryResetDialog = false">Cancel</v-btn>
          <v-btn color="error" variant="flat" :loading="resetting" @click="performFactoryReset">
            Reset Device
          </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>
    <!-- Import Config Confirmation Dialog -->
    <v-dialog v-model="showImportDialog" max-width="500">
      <v-card>
        <v-card-title>
          <v-icon icon="mdi-upload" class="mr-2" />
          Import Config
        </v-card-title>
        <v-card-text>
          <v-alert type="warning" variant="tonal" class="mb-4">
            This will overwrite your current settings with the imported config.
          </v-alert>
          <div class="text-body-2 mb-2">
            File: <strong>{{ importFileName }}</strong>
          </div>
          <div v-if="importData" class="text-body-2">
            Sections to import:
            <ul class="mt-1 ml-4">
              <li v-if="importData.config">Device settings</li>
              <li v-if="importData.processing">Processing settings</li>
              <li v-if="importData.palette">Palette calibration</li>
              <li v-if="importData.albums?.length">Album enabled/disabled state</li>
            </ul>
          </div>
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showImportDialog = false">Cancel</v-btn>
          <v-btn color="primary" variant="flat" @click="performImport"> Import </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>

    <v-snackbar v-model="snackbar" :color="snackbarColor" timeout="3000">
      {{ snackbarText }}
    </v-snackbar>
  </div>
</template>

<style scoped></style>
