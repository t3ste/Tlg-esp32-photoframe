<script setup>
import { ref } from "vue";
import { useAppStore } from "../stores";

const appStore = useAppStore();
const sleepDialog = ref(false);
const rotating = ref(false);

async function handleSleep() {
  await appStore.enterSleep();
  sleepDialog.value = false;
}

async function handleRotate() {
  rotating.value = true;
  await appStore.rotateNow();
  rotating.value = false;
}

function getBatteryColor(level) {
  if (level < 20) return "error";
  if (level < 50) return "warning";
  return "success";
}
</script>

<template>
  <v-app-bar color="primary" density="comfortable">
    <template #prepend>
      <v-img src="/favicon.svg" alt="PhotoFrame" width="40" height="40" class="ml-2 mr-n2" />
    </template>

    <v-app-bar-title class="ml-4">PhotoFrame Control Panel</v-app-bar-title>

    <template #append>
      <!-- Rotate Now Button -->
      <v-btn
        icon
        class="mr-2"
        :loading="rotating"
        title="Rotate to next image"
        @click="handleRotate"
      >
        <v-icon>mdi-rotate-right</v-icon>
      </v-btn>

      <!-- Sleep Button -->
      <v-btn icon class="mr-2" title="Enter Deep Sleep" @click="sleepDialog = true">
        <v-icon>mdi-sleep</v-icon>
      </v-btn>

      <!-- Battery Status -->
      <v-chip
        v-if="appStore.battery.connected"
        variant="flat"
        :color="getBatteryColor(appStore.battery.level)"
        class="text-white"
      >
        <v-icon :icon="appStore.battery.charging ? 'mdi-battery-charging' : 'mdi-battery'" start />
        {{ appStore.battery.level }}%
        <span v-if="appStore.battery.charging" class="ml-1">Charging</span>
      </v-chip>
      <v-chip v-else variant="flat" color="grey-darken-1" class="mr-2 text-white">
        <v-icon icon="mdi-power-plug-off" start />
        No Battery
      </v-chip>
    </template>
  </v-app-bar>

  <!-- Sleep Confirmation Dialog -->
  <v-dialog v-model="sleepDialog" max-width="400">
    <v-card>
      <v-card-title>Enter Deep Sleep?</v-card-title>
      <v-card-text>
        The device will enter deep sleep mode and stop responding until woken up.
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="sleepDialog = false"> Cancel </v-btn>
        <v-btn color="primary" @click="handleSleep"> Sleep </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>
</template>
