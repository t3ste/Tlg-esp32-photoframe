<script setup>
import { ref } from "vue";
import { useAppStore } from "../stores";

const appStore = useAppStore();

const newAlbumDialog = ref(false);
const newAlbumName = ref("");
const deleteAlbumDialog = ref(false);
const albumToDelete = ref(null);
const displayLoading = ref(false);
const displayDialog = ref(false);
const imageToDisplay = ref(null);
const deleteImageDialog = ref(false);
const imageToDelete = ref(null);

async function createAlbum() {
  if (newAlbumName.value.trim()) {
    await appStore.createAlbum(newAlbumName.value);
    newAlbumName.value = "";
    newAlbumDialog.value = false;
  }
}

function confirmDeleteAlbum(album) {
  albumToDelete.value = album;
  deleteAlbumDialog.value = true;
}

async function deleteAlbum() {
  if (albumToDelete.value) {
    await appStore.deleteAlbum(albumToDelete.value.name);
    albumToDelete.value = null;
    deleteAlbumDialog.value = false;
  }
}

function confirmDisplayImage(image) {
  imageToDisplay.value = image;
  displayDialog.value = true;
}

async function displayImage() {
  if (imageToDisplay.value) {
    displayDialog.value = false;
    displayLoading.value = true;
    await appStore.displayImage(imageToDisplay.value.album, imageToDisplay.value.filename);
    displayLoading.value = false;
    imageToDisplay.value = null;
  }
}

function confirmDeleteImage(image) {
  imageToDelete.value = image;
  deleteImageDialog.value = true;
}

async function deleteImage() {
  if (imageToDelete.value) {
    await appStore.deleteImage(imageToDelete.value.album, imageToDelete.value.filename);
    imageToDelete.value = null;
    deleteImageDialog.value = false;
  }
}

function getThumbnailUrl(image) {
  return `/api/image?filepath=${encodeURIComponent(image.album + "/" + image.thumbnail)}`;
}
</script>

<template>
  <v-card>
    <v-card-title class="d-flex align-center">
      <v-icon icon="mdi-image-multiple" class="mr-2" />
      Albums & Gallery
    </v-card-title>

    <v-card-text>
      <!-- Album List -->
      <div class="d-flex align-center mb-4">
        <span class="text-body-2 text-grey mr-4">
          ✓ = Enabled for auto-rotation • Click album to view images
        </span>
        <v-spacer />
        <v-btn color="primary" size="small" @click="newAlbumDialog = true">
          <v-icon icon="mdi-plus" start />
          New Album
        </v-btn>
      </div>

      <v-chip-group
        :model-value="appStore.selectedAlbum"
        mandatory
        selected-class="text-primary"
        @update:model-value="appStore.selectAlbum($event)"
      >
        <v-chip
          v-for="album in appStore.sortedAlbums"
          :key="album.name"
          :value="album.name"
          :prepend-icon="album.enabled ? 'mdi-check-circle' : 'mdi-circle-outline'"
          variant="outlined"
          class="mr-2"
        >
          {{ album.name }}
          <template #append>
            <v-btn
              v-if="album.name !== 'Default'"
              icon
              size="x-small"
              variant="text"
              @click.stop="confirmDeleteAlbum(album)"
            >
              <v-icon size="small"> mdi-close </v-icon>
            </v-btn>
          </template>
        </v-chip>
      </v-chip-group>

      <v-divider class="my-4" />

      <!-- Image Gallery -->
      <div class="d-flex align-center mb-4">
        <h3 class="text-h6">
          {{ appStore.selectedAlbum }}
        </h3>
        <v-spacer />
        <v-progress-circular v-if="displayLoading" indeterminate size="24" class="mr-2" />
        <span v-if="displayLoading" class="text-body-2 text-grey"> Updating display... </span>
      </div>

      <v-row v-if="appStore.currentAlbumImages.length > 0">
        <v-col
          v-for="image in appStore.currentAlbumImages"
          :key="image.filename"
          cols="6"
          sm="4"
          md="3"
          lg="2"
        >
          <v-card variant="outlined" class="image-card">
            <v-img
              :src="getThumbnailUrl(image)"
              :alt="image.filename"
              aspect-ratio="1"
              contain
              class="cursor-pointer bg-grey-lighten-3"
              @click="confirmDisplayImage(image)"
            >
              <template #placeholder>
                <div class="d-flex align-center justify-center fill-height">
                  <v-progress-circular indeterminate color="grey-lighten-4" />
                </div>
              </template>
            </v-img>
            <v-card-actions class="pa-1">
              <span class="text-caption text-truncate flex-grow-1 px-1">
                {{ image.filename }}
              </span>
              <v-btn
                icon
                size="x-small"
                variant="text"
                color="error"
                @click="confirmDeleteImage(image)"
              >
                <v-icon size="small"> mdi-delete </v-icon>
              </v-btn>
            </v-card-actions>
          </v-card>
        </v-col>
      </v-row>

      <v-alert v-else type="info" variant="tonal" class="mt-4">
        No images in this album. Upload images to get started.
      </v-alert>
    </v-card-text>
  </v-card>

  <!-- New Album Dialog -->
  <v-dialog v-model="newAlbumDialog" max-width="400">
    <v-card>
      <v-card-title>Create New Album</v-card-title>
      <v-card-text>
        <v-text-field
          v-model="newAlbumName"
          label="Album Name"
          autofocus
          @keyup.enter="createAlbum"
        />
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="newAlbumDialog = false"> Cancel </v-btn>
        <v-btn color="primary" @click="createAlbum"> Create </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <!-- Delete Album Dialog -->
  <v-dialog v-model="deleteAlbumDialog" max-width="400">
    <v-card>
      <v-card-title>Delete Album?</v-card-title>
      <v-card-text>
        Are you sure you want to delete "{{ albumToDelete?.name }}"? This will also delete all
        images in the album.
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="deleteAlbumDialog = false"> Cancel </v-btn>
        <v-btn color="error" @click="deleteAlbum"> Delete </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <!-- Display Image Dialog -->
  <v-dialog v-model="displayDialog" max-width="400">
    <v-card>
      <v-card-title>
        <v-icon icon="mdi-monitor" class="mr-2" />
        Display Image?
      </v-card-title>
      <v-card-text> Show "{{ imageToDisplay?.filename }}" on the e-paper display? </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="displayDialog = false"> Cancel </v-btn>
        <v-btn color="primary" @click="displayImage"> Display </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <!-- Delete Image Dialog -->
  <v-dialog v-model="deleteImageDialog" max-width="400">
    <v-card>
      <v-card-title>
        <v-icon icon="mdi-delete" color="error" class="mr-2" />
        Delete Image?
      </v-card-title>
      <v-card-text> Are you sure you want to delete "{{ imageToDelete?.filename }}"? </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="deleteImageDialog = false"> Cancel </v-btn>
        <v-btn color="error" @click="deleteImage"> Delete </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>
</template>

<style scoped>
.image-card {
  transition:
    transform 0.2s,
    box-shadow 0.2s;
}
.image-card:hover {
  transform: translateY(-2px);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
}
.cursor-pointer {
  cursor: pointer;
}
</style>
