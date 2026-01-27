<script setup>
import { onMounted } from "vue";
import { useAppStore, useSettingsStore } from "../stores";
import AppHeader from "../components/AppHeader.vue";
import AlbumGallery from "../components/AlbumGallery.vue";
import ImageUpload from "../components/ImageUpload.vue";
import SettingsPanel from "../components/SettingsPanel.vue";
import OtaUpdate from "../components/OtaUpdate.vue";

const appStore = useAppStore();
const settingsStore = useSettingsStore();

onMounted(async () => {
  await Promise.all([
    appStore.loadBatteryStatus(),
    appStore.loadAlbums(),
    settingsStore.loadSettings(),
    settingsStore.loadDeviceSettings(),
    settingsStore.loadPalette(),
  ]);
  appStore.selectAlbum("Default");

  // Refresh battery every 30s
  setInterval(() => appStore.loadBatteryStatus(), 30000);
});
</script>

<template>
  <v-app>
    <AppHeader />

    <v-main class="bg-grey-lighten-4">
      <v-container class="py-6" style="max-width: 1200px">
        <AlbumGallery />

        <ImageUpload class="mt-6" />

        <SettingsPanel class="mt-6" />

        <OtaUpdate class="mt-6" />
      </v-container>
    </v-main>

    <v-footer app class="text-center d-flex justify-center">
      <span class="text-body-2 text-grey">ESP32-S3 PhotoFrame v1.0</span>
    </v-footer>
  </v-app>
</template>
