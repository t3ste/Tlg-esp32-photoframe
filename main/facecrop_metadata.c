#include "facecrop_metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "facecrop_metadata";

// A hand-sized JSON sidecar (see docs/FACE_CROP.md's example) is well under
// a few KB - reject anything wildly out of shape rather than allocating an
// unbounded buffer for a malformed or hostile file.
#define FACECROP_JSON_MAX_SIZE 16384

// Derives "<dir>/<name>.facecrop.json" from an original image path
// "<dir>/<name>.<ext>" - mirrors process-cli's metadataPathFor() (see
// process-cli/face-crop/metadata.js): strip the extension, append the
// fixed suffix.
static bool derive_metadata_path(const char *original_path, char *out, size_t out_size)
{
    strncpy(out, original_path, out_size - 1);
    out[out_size - 1] = '\0';

    char *ext = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    // A dot belonging to a directory component (not the filename's own
    // extension) doesn't count - e.g. "/sd/Al.bum/name" with no extension.
    if (!ext || (slash && ext < slash)) {
        return false;
    }

    size_t base_len = (size_t) (ext - out);
    if (base_len + strlen(".facecrop.json") + 1 > out_size) {
        return false;
    }
    strcpy(out + base_len, ".facecrop.json");
    return true;
}

bool facecrop_read_recommended_crop(const char *original_path, image_crop_rect_t *out)
{
    char metadata_path[320];
    if (!derive_metadata_path(original_path, metadata_path, sizeof(metadata_path))) {
        return false;
    }

    FILE *fp = fopen(metadata_path, "rb");
    if (!fp) {
        return false;  // No sidecar - not an error, just "unavailable"
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > FACECROP_JSON_MAX_SIZE) {
        fclose(fp);
        return false;
    }

    char *buffer = malloc((size_t) size + 1);
    if (!buffer) {
        fclose(fp);
        return false;
    }
    size_t read_bytes = fread(buffer, 1, (size_t) size, fp);
    fclose(fp);
    buffer[read_bytes] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse %s", metadata_path);
        return false;
    }

    bool ok = false;
    cJSON *crop = cJSON_GetObjectItem(root, "recommended_crop");
    if (cJSON_IsObject(crop)) {
        cJSON *x = cJSON_GetObjectItem(crop, "x");
        cJSON *y = cJSON_GetObjectItem(crop, "y");
        cJSON *w = cJSON_GetObjectItem(crop, "w");
        cJSON *h = cJSON_GetObjectItem(crop, "h");
        if (cJSON_IsNumber(x) && cJSON_IsNumber(y) && cJSON_IsNumber(w) && cJSON_IsNumber(h) &&
            w->valuedouble > 0 && h->valuedouble > 0) {
            out->x = (int) x->valuedouble;
            out->y = (int) y->valuedouble;
            out->w = (int) w->valuedouble;
            out->h = (int) h->valuedouble;
            ok = true;
        }
    }

    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "%s has no valid recommended_crop", metadata_path);
    }
    return ok;
}
