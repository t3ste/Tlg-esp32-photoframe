<script setup>
import { ref, computed } from "vue";
import { getDitherOptions, getPresetOptions } from "epaper-image-convert";
import { useSettingsStore } from "../stores";

const settingsStore = useSettingsStore();

const tab = computed({
  get: () => settingsStore.activeSettingsTab,
  set: (val) => (settingsStore.activeSettingsTab = val),
});

const orientationOptions = [
  { title: "Landscape (800×480)", value: "landscape" },
  { title: "Portrait (480×800)", value: "portrait" },
];

const rotationModeOptions = [
  { title: "SD Card - Rotate through images", value: "sdcard" },
  { title: "URL - Fetch image from URL", value: "url" },
];
const saving = ref(false);
const saveSuccess = ref(false);

const presetOptions = [
  ...getPresetOptions(),
  { value: "custom", title: "Custom", description: "Manually adjusted parameters" },
];

const toneModeOptions = [
  { title: "Contrast", value: "contrast" },
  { title: "S-Curve", value: "scurve" },
];

const colorMethodOptions = [
  { title: "Simple RGB", value: "rgb" },
  { title: "LAB Color Space", value: "lab" },
];

const ditherOptions = getDitherOptions();

const isScurveMode = computed(() => settingsStore.params.toneMode === "scurve");

function onPresetChange(preset) {
  if (preset !== "custom") {
    settingsStore.applyPreset(preset);
  }
}

const saveMessage = ref("");

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
    saveMessage.value = deviceResult.message || "Settings saved!";
    setTimeout(() => (saveSuccess.value = false), 3000);
  }
}
</script>

<template>
  <v-card style="overflow: visible">
    <v-card-title class="d-flex align-center">
      <v-icon icon="mdi-cog" class="mr-2" />
      Settings
    </v-card-title>

    <v-tabs v-model="tab" color="primary" show-arrows density="compact">
      <v-tab value="general"> General </v-tab>
      <v-tab value="autoRotate"> Auto Rotate </v-tab>
      <v-tab value="power"> Power </v-tab>
      <v-tab value="homeAssistant"> Home Assistant </v-tab>
      <v-tab value="processing"> Processing </v-tab>
      <v-tab value="calibration"> Palette Calibration </v-tab>
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
                hint="Used for mDNS hostname (e.g., 'Living Room' → living-room.local)"
                persistent-hint
              />
            </v-col>

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
          </v-row>

          <v-row>
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
        </v-tabs-window-item>

        <!-- Auto Rotate Tab -->
        <v-tabs-window-item value="autoRotate">
          <v-switch
            v-model="settingsStore.deviceSettings.autoRotate"
            label="Enable Auto-Rotate"
            color="primary"
            class="mb-4"
          />

          <v-row>
            <v-col cols="6" md="3">
              <v-text-field
                v-model.number="settingsStore.deviceSettings.rotateHours"
                label="Hours"
                type="number"
                :min="0"
                :max="23"
                variant="outlined"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />
            </v-col>
            <v-col cols="6" md="3">
              <v-text-field
                v-model.number="settingsStore.deviceSettings.rotateMinutes"
                label="Minutes"
                type="number"
                :min="0"
                :max="59"
                variant="outlined"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />
            </v-col>
          </v-row>

          <v-alert type="info" variant="tonal" density="compact" class="mb-4">
            Rotation aligns to clock boundaries. 1 hour rotates at 1:00, 2:00, etc.
          </v-alert>

          <v-select
            v-model="settingsStore.deviceSettings.rotationMode"
            :items="rotationModeOptions"
            item-title="title"
            item-value="value"
            label="Rotation Mode"
            variant="outlined"
            class="mb-4"
            :disabled="!settingsStore.deviceSettings.autoRotate"
          />

          <v-expand-transition>
            <v-card
              v-if="settingsStore.deviceSettings.rotationMode === 'url'"
              variant="tonal"
              class="mb-4"
            >
              <v-card-text>
                <v-text-field
                  v-model="settingsStore.deviceSettings.imageUrl"
                  label="Image URL"
                  variant="outlined"
                  hint="Default provides random images"
                  persistent-hint
                  class="mb-4"
                />

                <v-checkbox
                  v-model="settingsStore.deviceSettings.saveDownloadedImages"
                  label="Save downloaded images to Downloads album"
                  color="primary"
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

          <v-divider class="my-4" />

          <h3 class="text-subtitle-1 mb-4">Sleep Schedule</h3>

          <v-switch
            v-model="settingsStore.deviceSettings.sleepScheduleEnabled"
            label="Enable Sleep Schedule"
            color="primary"
            class="mb-4"
          />

          <v-row>
            <v-col cols="6" md="3">
              <v-text-field
                v-model="settingsStore.deviceSettings.sleepScheduleStart"
                label="From"
                type="time"
                variant="outlined"
                :disabled="!settingsStore.deviceSettings.sleepScheduleEnabled"
              />
            </v-col>
            <v-col cols="6" md="3">
              <v-text-field
                v-model="settingsStore.deviceSettings.sleepScheduleEnd"
                label="To"
                type="time"
                variant="outlined"
                :disabled="!settingsStore.deviceSettings.sleepScheduleEnabled"
              />
            </v-col>
          </v-row>

          <v-alert type="info" variant="tonal" density="compact">
            Images won't rotate during this period. Useful for night hours.
          </v-alert>
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
        </v-tabs-window-item>

        <!-- Home Assistant Tab -->
        <v-tabs-window-item class="mt-2" value="homeAssistant">
          <v-text-field
            v-model="settingsStore.deviceSettings.haUrl"
            label="Home Assistant URL"
            variant="outlined"
            placeholder="http://homeassistant.local:8123"
            hint="Configure for dynamic image serving and battery level reporting"
            persistent-hint
          />
        </v-tabs-window-item>

        <!-- Processing Tab -->
        <v-tabs-window-item value="processing">
          <div class="pa-4">
            <!-- Preset Selection -->
            <v-row>
              <v-col cols="12">
                <v-card variant="outlined" class="mb-6">
                  <v-card-subtitle class="pt-3"> Processing Preset </v-card-subtitle>
                  <v-card-text>
                    <v-btn-toggle
                      :model-value="settingsStore.preset"
                      mandatory
                      color="primary"
                      variant="outlined"
                      @update:model-value="onPresetChange"
                    >
                      <v-btn v-for="p in presetOptions" :key="p.value" :value="p.value">
                        {{ p.title }}
                      </v-btn>
                    </v-btn-toggle>
                  </v-card-text>
                </v-card>
              </v-col>

              <!-- Dither Algorithm -->
              <v-col cols="12" md="4">
                <v-select
                  v-model="settingsStore.params.ditherAlgorithm"
                  :items="ditherOptions"
                  item-title="title"
                  item-value="value"
                  label="Dithering Algorithm"
                  variant="outlined"
                  density="compact"
                />
              </v-col>

              <v-col cols="12" md="4">
                <v-select
                  v-model="settingsStore.params.colorMethod"
                  :items="colorMethodOptions"
                  item-title="title"
                  item-value="value"
                  label="Color Matching"
                  variant="outlined"
                  density="compact"
                />
              </v-col>
            </v-row>

            <v-row>
              <!-- Exposure -->
              <v-col cols="12" md="4">
                <v-slider
                  v-model="settingsStore.params.exposure"
                  :min="0.5"
                  :max="2.0"
                  :step="0.01"
                  label="Exposure"
                  thumb-label
                  color="primary"
                >
                  <template #append>
                    <span class="text-body-2">{{ settingsStore.params.exposure.toFixed(2) }}</span>
                  </template>
                </v-slider>
              </v-col>

              <!-- Saturation -->
              <v-col cols="12" md="4">
                <v-slider
                  v-model="settingsStore.params.saturation"
                  :min="0.5"
                  :max="2.0"
                  :step="0.01"
                  label="Saturation"
                  thumb-label
                  color="primary"
                >
                  <template #append>
                    <span class="text-body-2">{{
                      settingsStore.params.saturation.toFixed(2)
                    }}</span>
                  </template>
                </v-slider>
              </v-col>

              <!-- Compress Dynamic Range -->
              <v-col cols="12" md="4">
                <v-checkbox
                  v-model="settingsStore.params.compressDynamicRange"
                  label="Compress Dynamic Range"
                  hint="Map brightness to display's actual white point"
                  persistent-hint
                  color="primary"
                />
              </v-col>
            </v-row>

            <!-- Tone Mode -->
            <v-row>
              <v-col cols="12" md="4">
                <v-select
                  v-model="settingsStore.params.toneMode"
                  :items="toneModeOptions"
                  item-title="title"
                  item-value="value"
                  label="Tone Mapping"
                  variant="outlined"
                  density="compact"
                />
              </v-col>

              <!-- Contrast (for contrast mode) -->
              <v-col v-if="!isScurveMode" cols="12" md="4">
                <v-slider
                  v-model="settingsStore.params.contrast"
                  :min="0.5"
                  :max="2.0"
                  :step="0.01"
                  label="Contrast"
                  thumb-label
                  color="primary"
                >
                  <template #append>
                    <span class="text-body-2">{{ settingsStore.params.contrast.toFixed(2) }}</span>
                  </template>
                </v-slider>
              </v-col>
            </v-row>

            <!-- S-Curve Controls -->
            <v-expand-transition>
              <v-card v-if="isScurveMode" variant="tonal" class="mt-4">
                <v-card-subtitle class="pt-3"> S-Curve Parameters </v-card-subtitle>
                <v-card-text>
                  <v-row>
                    <v-col cols="12" md="6">
                      <v-slider
                        v-model="settingsStore.params.strength"
                        :min="0"
                        :max="1"
                        :step="0.01"
                        label="Strength"
                        thumb-label
                        color="primary"
                      >
                        <template #append>
                          <span class="text-body-2">{{
                            settingsStore.params.strength.toFixed(2)
                          }}</span>
                        </template>
                      </v-slider>
                    </v-col>

                    <v-col cols="12" md="6">
                      <v-slider
                        v-model="settingsStore.params.shadowBoost"
                        :min="0"
                        :max="1"
                        :step="0.01"
                        label="Shadow Boost"
                        thumb-label
                        color="primary"
                      >
                        <template #append>
                          <span class="text-body-2">{{
                            settingsStore.params.shadowBoost.toFixed(2)
                          }}</span>
                        </template>
                      </v-slider>
                    </v-col>

                    <v-col cols="12" md="6">
                      <v-slider
                        v-model="settingsStore.params.highlightCompress"
                        :min="0.5"
                        :max="5"
                        :step="0.01"
                        label="Highlight Compress"
                        thumb-label
                        color="primary"
                      >
                        <template #append>
                          <span class="text-body-2">{{
                            settingsStore.params.highlightCompress.toFixed(2)
                          }}</span>
                        </template>
                      </v-slider>
                    </v-col>

                    <v-col cols="12" md="6">
                      <v-slider
                        v-model="settingsStore.params.midpoint"
                        :min="0.3"
                        :max="0.7"
                        :step="0.01"
                        label="Midpoint"
                        thumb-label
                        color="primary"
                      >
                        <template #append>
                          <span class="text-body-2">{{
                            settingsStore.params.midpoint.toFixed(2)
                          }}</span>
                        </template>
                      </v-slider>
                    </v-col>
                  </v-row>
                </v-card-text>
              </v-card>
            </v-expand-transition>
          </div>
        </v-tabs-window-item>

        <!-- Calibration Tab -->
        <v-tabs-window-item value="calibration">
          <v-alert type="info" variant="tonal" class="mb-4">
            Calibrate the measured color palette for your specific E6 display batch. This improves
            color accuracy during image processing.
          </v-alert>

          <h3 class="text-h6 mb-4">Current Palette</h3>

          <v-row>
            <v-col
              v-for="(color, name) in settingsStore.palette"
              :key="name"
              cols="6"
              md="4"
              lg="2"
            >
              <v-card variant="outlined">
                <div
                  class="color-swatch"
                  :style="{ backgroundColor: `rgb(${color.r}, ${color.g}, ${color.b})` }"
                />
                <v-card-text class="pa-2">
                  <div class="text-subtitle-2 text-capitalize mb-2">
                    {{ name }}
                  </div>
                  <v-text-field
                    v-model.number="color.r"
                    label="R"
                    type="number"
                    density="compact"
                    variant="outlined"
                    :min="0"
                    :max="255"
                    class="mb-1"
                  />
                  <v-text-field
                    v-model.number="color.g"
                    label="G"
                    type="number"
                    density="compact"
                    variant="outlined"
                    :min="0"
                    :max="255"
                    class="mb-1"
                  />
                  <v-text-field
                    v-model.number="color.b"
                    label="B"
                    type="number"
                    density="compact"
                    variant="outlined"
                    :min="0"
                    :max="255"
                  />
                </v-card-text>
              </v-card>
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
      </v-fade-transition>
      <v-btn color="primary" :loading="saving" @click="saveSettings">
        <v-icon icon="mdi-content-save" start />
        Save Settings
      </v-btn>
    </v-card-actions>
  </v-card>
</template>

<style scoped>
.color-swatch {
  height: 60px;
  border-bottom: 1px solid rgba(0, 0, 0, 0.12);
}
</style>
