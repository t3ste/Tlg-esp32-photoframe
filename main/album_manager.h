#ifndef ALBUM_MANAGER_H
#define ALBUM_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t album_manager_init(void);
esp_err_t album_manager_ensure_default_album(void);
esp_err_t album_manager_list_albums(char ***albums, int *count);
void album_manager_free_album_list(char **albums, int count);
esp_err_t album_manager_create_album(const char *album_name);
esp_err_t album_manager_delete_album(const char *album_name);

/**
 * @brief Web UI maintenance action: move every loose "<name>.cover.<ext>"
 * in any album into that album's "crop" subdirectory (created if missing) -
 * the layout the Cover/Fit variant-selection feature expects (see
 * docs/FACE_CROP.md). Applies to every album, not just enabled ones.
 *
 * @param out_moved_count Optional (may be NULL) - set to how many files
 *   were moved.
 */
esp_err_t album_manager_organize_crop_variants(int *out_moved_count);
esp_err_t album_manager_set_album_enabled(const char *album_name, bool enabled);
bool album_manager_is_album_enabled(const char *album_name);
esp_err_t album_manager_get_enabled_albums(char ***albums, int *count);
esp_err_t album_manager_get_album_path(const char *album_name, char *path, size_t path_len);
bool album_manager_album_exists(const char *album_name);

#endif
