#include "telegram_bot.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board_hal.h"
#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_processor.h"
#include "processing_settings.h"
#include "wifi_manager.h"

static const char *TAG = "telegram_bot";

// Cap on the accumulated getUpdates/getFile response body. A batch of
// TELEGRAM_MAX_UPDATES_PER_POLL updates comfortably fits well under this.
#define TELEGRAM_MAX_RESPONSE_BYTES (256 * 1024)

// Pending "/" commands queued by the last poll, executed (and replied to)
// after the image has been displayed but before deep sleep. Reset on every
// boot (deep sleep restarts the app), which matches the intended lifetime:
// queue -> run -> sleep, once per wake.
static char s_pending_commands[TELEGRAM_MAX_PENDING_COMMANDS][TELEGRAM_COMMAND_MAX_LEN];
static int s_pending_command_count = 0;

// ----------------------------------------------------------------------------
// Small HTTP helpers
// ----------------------------------------------------------------------------

// Growing in-memory buffer used to capture GET response bodies (getUpdates,
// getFile). Capped at TELEGRAM_MAX_RESPONSE_BYTES to bound worst-case heap use.
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} http_body_buf_t;

static esp_err_t body_capture_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    http_body_buf_t *ctx = (http_body_buf_t *) evt->user_data;
    if (ctx->overflow || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t need = ctx->len + (size_t) evt->data_len + 1;
    if (need > TELEGRAM_MAX_RESPONSE_BYTES) {
        ESP_LOGW(TAG, "Response body exceeds %d bytes cap, truncating", TELEGRAM_MAX_RESPONSE_BYTES);
        ctx->overflow = true;
        return ESP_OK;
    }
    if (need > ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 4096;
        while (new_cap < need) {
            new_cap *= 2;
        }
        char *grown = realloc(ctx->buf, new_cap);
        if (!grown) {
            ESP_LOGE(TAG, "Out of memory growing response buffer to %zu bytes", new_cap);
            ctx->overflow = true;
            return ESP_OK;
        }
        ctx->buf = grown;
        ctx->cap = new_cap;
    }

    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

// Performs a GET request and returns the response body (caller frees with
// free()). *out_body is NULL on any failure.
static esp_err_t telegram_http_get(const char *url, int timeout_ms, char **out_body,
                                   size_t *out_len)
{
    *out_body = NULL;
    if (out_len) {
        *out_len = 0;
    }

    http_body_buf_t ctx = {0};

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = body_capture_handler,
        .user_data = &ctx,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for GET %s", url);
        free(ctx.buf);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        free(ctx.buf);
        return err;
    }
    if (status != 200 || ctx.overflow || !ctx.buf) {
        ESP_LOGE(TAG, "GET %s returned HTTP %d%s", url, status, ctx.overflow ? " (truncated)" : "");
        free(ctx.buf);
        return ESP_FAIL;
    }

    *out_body = ctx.buf;
    if (out_len) {
        *out_len = ctx.len;
    }
    return ESP_OK;
}

// Downloads raw bytes (a Telegram file) straight to a local file.
typedef struct {
    FILE *file;
    int total_bytes;
} file_download_ctx_t;

static esp_err_t file_download_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    file_download_ctx_t *ctx = (file_download_ctx_t *) evt->user_data;
    if (ctx->file && evt->data_len > 0) {
        fwrite(evt->data, 1, evt->data_len, ctx->file);
        ctx->total_bytes += evt->data_len;
    }
    return ESP_OK;
}

static esp_err_t telegram_download_to_file(const char *url, const char *local_path)
{
    FILE *f = fopen(local_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", local_path);
        return ESP_FAIL;
    }

    file_download_ctx_t ctx = {.file = f, .total_bytes = 0};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
        .event_handler = file_download_handler,
        .user_data = &ctx,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f);
        unlink(local_path);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    fclose(f);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx.total_bytes <= 0) {
        ESP_LOGW(TAG, "File download failed (err=%s, status=%d, bytes=%d)", esp_err_to_name(err),
                 status, ctx.total_bytes);
        unlink(local_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes to %s", ctx.total_bytes, local_path);
    return ESP_OK;
}

// ----------------------------------------------------------------------------
// Telegram Bot API method helpers
// ----------------------------------------------------------------------------

static void build_api_url(const char *method, char *out, size_t out_len)
{
    snprintf(out, out_len, TELEGRAM_API_BASE_FMT, config_manager_get_telegram_bot_token(), method);
}

esp_err_t telegram_bot_send_message(const char *text)
{
    if (!config_manager_telegram_is_configured() || !text) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "chat_id", config_manager_get_telegram_chat_id());
    cJSON_AddStringToObject(body, "text", text);
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    char url[256];
    build_api_url("sendMessage", url, sizeof(url));

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "sendMessage failed (err=%s, status=%d)", esp_err_to_name(err), status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Resolves a Telegram file_id to a downloadable file_path via getFile.
static esp_err_t telegram_get_file_path(const char *file_id, char *out_path, size_t out_len)
{
    char base[256];
    build_api_url("getFile", base, sizeof(base));
    char url[320];
    snprintf(url, sizeof(url), "%s?file_id=%s", base, file_id);

    char *body = NULL;
    esp_err_t err = telegram_http_get(url, TELEGRAM_HTTP_TIMEOUT_MS, &body, NULL);
    if (err != ESP_OK || !body) {
        free(body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "getFile: failed to parse JSON response");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (ok && cJSON_IsTrue(ok) && result) {
        cJSON *fp = cJSON_GetObjectItem(result, "file_path");
        if (fp && cJSON_IsString(fp)) {
            strncpy(out_path, fp->valuestring, out_len - 1);
            out_path[out_len - 1] = '\0';
            ret = ESP_OK;
        }
    } else {
        cJSON *desc = cJSON_GetObjectItem(root, "description");
        ESP_LOGW(TAG, "getFile failed: %s",
                 (desc && cJSON_IsString(desc)) ? desc->valuestring : "unknown error");
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t telegram_download_file_id(const char *file_id, const char *local_path)
{
    char tg_file_path[256];
    if (telegram_get_file_path(file_id, tg_file_path, sizeof(tg_file_path)) != ESP_OK) {
        return ESP_FAIL;
    }

    char url[400];
    snprintf(url, sizeof(url), "https://" TELEGRAM_API_HOST "/file/bot%s/%s",
             config_manager_get_telegram_bot_token(), tg_file_path);
    return telegram_download_to_file(url, local_path);
}

// ----------------------------------------------------------------------------
// Progressive-JPEG detection
// ----------------------------------------------------------------------------

// Telegram re-encodes compressed "photo" messages as progressive JPEG, which
// this firmware's JPEG decoder (tjpgd-based) cannot decode (only baseline/
// sequential SOF0/SOF1). Scan the JPEG marker stream for SOF0/1 (baseline,
// OK) vs SOF2/3 (progressive/lossless, unsupported) so a bad download can be
// treated as a failure and trigger the same progressive size fallback used
// for "file too large". Only the first chunk is scanned - SOF markers appear
// before the entropy-coded scan data, which starts well within this window
// for realistic Telegram photos/thumbnails.
#define JPEG_HEADER_SCAN_BYTES 65536

static bool jpeg_file_is_progressive(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    uint8_t *buf = malloc(JPEG_HEADER_SCAN_BYTES);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t n = fread(buf, 1, JPEG_HEADER_SCAN_BYTES, f);
    fclose(f);

    bool is_progressive = false;
    if (n >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
        size_t i = 2;
        while (i + 4 <= n) {
            if (buf[i] != 0xFF) {
                i++;
                continue;
            }
            uint8_t marker = buf[i + 1];
            // Markers with no payload (RSTn, SOI dup, TEM, padding 0xFF).
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9) || marker == 0xFF) {
                i += 2;
                continue;
            }
            uint16_t seg_len = (uint16_t) ((buf[i + 2] << 8) | buf[i + 3]);
            if (marker == 0xC0 || marker == 0xC1) {  // SOF0 / SOF1: baseline / extended sequential
                break;
            }
            if (marker == 0xC2 || marker == 0xC3) {  // SOF2 / SOF3: progressive / lossless
                is_progressive = true;
                break;
            }
            if (marker == 0xDA) {  // SOS: entropy-coded data follows, no SOF found before it
                break;
            }
            if (seg_len < 2) {
                break;  // malformed segment length, stop scanning
            }
            i += 2 + seg_len;
        }
    }

    free(buf);
    return is_progressive;
}

// ----------------------------------------------------------------------------
// Image download with progressive-size fallback
// ----------------------------------------------------------------------------

static esp_err_t make_unique_telegram_path(const char *ext, char *out, size_t out_len)
{
    time_t now = time(NULL);
    for (int suffix = 0; suffix < 100; suffix++) {
        if (suffix == 0) {
            snprintf(out, out_len, "%s/img_%lld.%s", TELEGRAM_DOWNLOAD_DIRECTORY, (long long) now,
                     ext);
        } else {
            snprintf(out, out_len, "%s/img_%lld_%d.%s", TELEGRAM_DOWNLOAD_DIRECTORY,
                     (long long) now, suffix, ext);
        }
        struct stat st;
        if (stat(out, &st) != 0) {
            return ESP_OK;  // path is free
        }
    }
    ESP_LOGE(TAG, "Could not find a free filename under %s", TELEGRAM_DOWNLOAD_DIRECTORY);
    return ESP_FAIL;
}

// Downloads a Telegram "photo" (array of re-encoded resolutions, ascending
// size) trying the largest first. Falls back to the next smaller size when
// the current one is too large for getFile, fails to download, or turns out
// to be a progressive JPEG we can't decode - until one succeeds or the
// smallest size has also failed.
static esp_err_t download_photo_with_fallback(cJSON *photo_array, char *out_path,
                                               size_t out_path_len)
{
    int n = cJSON_GetArraySize(photo_array);
    for (int i = n - 1; i >= 0; i--) {
        cJSON *size_obj = cJSON_GetArrayItem(photo_array, i);
        cJSON *file_id_item = cJSON_GetObjectItem(size_obj, "file_id");
        if (!file_id_item || !cJSON_IsString(file_id_item)) {
            continue;
        }

        cJSON *fsize_item = cJSON_GetObjectItem(size_obj, "file_size");
        int approx_kb =
            (fsize_item && cJSON_IsNumber(fsize_item)) ? (int) (fsize_item->valuedouble / 1024) : -1;
        ESP_LOGI(TAG, "Trying Telegram photo size %d/%d (~%d KB)", i + 1, n, approx_kb);

        if (make_unique_telegram_path("jpg", out_path, out_path_len) != ESP_OK) {
            return ESP_FAIL;
        }

        if (telegram_download_file_id(file_id_item->valuestring, out_path) != ESP_OK) {
            ESP_LOGW(TAG, "Size %d/%d failed to download, falling back to smaller", i + 1, n);
            continue;
        }

        if (jpeg_file_is_progressive(out_path)) {
            ESP_LOGW(TAG,
                     "Size %d/%d is a progressive JPEG (unsupported by decoder), falling back to "
                     "smaller",
                     i + 1, n);
            unlink(out_path);
            continue;
        }

        return ESP_OK;
    }

    ESP_LOGE(TAG, "All %d photo size(s) failed (progressive fallback exhausted)", n);
    return ESP_FAIL;
}

// Documents (files sent "as file") are not re-encoded by Telegram, so they
// keep their original format/encoding and there is only a single size - no
// fallback ladder to walk, just one attempt.
static bool document_pick_extension(cJSON *document, const char **out_ext)
{
    cJSON *mime = cJSON_GetObjectItem(document, "mime_type");
    cJSON *fname = cJSON_GetObjectItem(document, "file_name");
    const char *ext = NULL;

    if (mime && cJSON_IsString(mime)) {
        const char *m = mime->valuestring;
        if (strcmp(m, "image/jpeg") == 0) {
            ext = "jpg";
        } else if (strcmp(m, "image/png") == 0) {
            ext = "png";
        } else if (strcmp(m, "image/bmp") == 0 || strcmp(m, "image/x-ms-bmp") == 0) {
            ext = "bmp";
        }
    }
    if (!ext && fname && cJSON_IsString(fname)) {
        const char *dot = strrchr(fname->valuestring, '.');
        if (dot) {
            if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
                ext = "jpg";
            } else if (strcasecmp(dot, ".png") == 0) {
                ext = "png";
            } else if (strcasecmp(dot, ".bmp") == 0) {
                ext = "bmp";
            } else if (strcasecmp(dot, ".epdgz") == 0) {
                ext = "epdgz";
            }
        }
    }

    *out_ext = ext;
    return ext != NULL;
}

static esp_err_t download_document_image(cJSON *document, char *out_path, size_t out_path_len)
{
    cJSON *file_id_item = cJSON_GetObjectItem(document, "file_id");
    if (!file_id_item || !cJSON_IsString(file_id_item)) {
        return ESP_FAIL;
    }

    const char *ext = NULL;
    if (!document_pick_extension(document, &ext)) {
        ESP_LOGW(TAG, "Document has an unsupported format, skipping");
        return ESP_FAIL;
    }

    if (make_unique_telegram_path(ext, out_path, out_path_len) != ESP_OK) {
        return ESP_FAIL;
    }

    if (telegram_download_file_id(file_id_item->valuestring, out_path) != ESP_OK) {
        return ESP_FAIL;
    }

    if (strcmp(ext, "jpg") == 0 && jpeg_file_is_progressive(out_path)) {
        ESP_LOGW(TAG, "Document JPEG is progressive (unsupported by decoder)");
        unlink(out_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Runs the downloaded image through the existing processing pipeline (same
// format handling as fetch_and_save_image_from_url in utils.c: EPDGZ/BMP are
// already display-ready, PNG/JPG go through image_processor_process) and
// shows it. The original file under TELEGRAM_DOWNLOAD_DIRECTORY is left in
// place for later rotation cycles.
static esp_err_t process_and_display_telegram_image(const char *path)
{
    image_format_t format = image_processor_detect_format(path);

    if (format == IMAGE_FORMAT_EPD_GZ || format == IMAGE_FORMAT_BMP) {
        return display_manager_show_image(path);
    }

    if (format != IMAGE_FORMAT_PNG && format != IMAGE_FORMAT_JPG) {
        ESP_LOGE(TAG, "Unsupported/undetected image format for %s", path);
        return ESP_FAIL;
    }

    if (format == IMAGE_FORMAT_PNG && image_processor_is_processed(path)) {
        return display_manager_show_image(path);
    }

    dither_algorithm_t algo = processing_settings_get_dithering_algorithm();
    esp_err_t err = image_processor_process(path, CURRENT_PNG_PATH, algo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to process Telegram image %s: %s", path, esp_err_to_name(err));
        return err;
    }

    return display_manager_show_image(CURRENT_PNG_PATH);
}

// ----------------------------------------------------------------------------
// Update parsing helpers
// ----------------------------------------------------------------------------

static cJSON *get_message(cJSON *update_item)
{
    return cJSON_GetObjectItem(update_item, "message");
}

static bool message_from_allowed_chat(cJSON *message, const char *allowed_chat_id)
{
    if (!message || !allowed_chat_id || allowed_chat_id[0] == '\0') {
        return false;
    }
    cJSON *chat = cJSON_GetObjectItem(message, "chat");
    if (!chat) {
        return false;
    }
    cJSON *id_item = cJSON_GetObjectItem(chat, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        return false;
    }

    char id_str[24];
    snprintf(id_str, sizeof(id_str), "%lld", (long long) id_item->valuedouble);
    return strcmp(id_str, allowed_chat_id) == 0;
}

static const char *get_text(cJSON *message)
{
    cJSON *text_item = cJSON_GetObjectItem(message, "text");
    if (text_item && cJSON_IsString(text_item)) {
        return text_item->valuestring;
    }
    return NULL;
}

static void queue_command(const char *text)
{
    if (s_pending_command_count >= TELEGRAM_MAX_PENDING_COMMANDS) {
        ESP_LOGW(TAG, "Pending command queue full, dropping: %s", text);
        return;
    }
    strncpy(s_pending_commands[s_pending_command_count], text, TELEGRAM_COMMAND_MAX_LEN - 1);
    s_pending_commands[s_pending_command_count][TELEGRAM_COMMAND_MAX_LEN - 1] = '\0';
    s_pending_command_count++;
    ESP_LOGI(TAG, "Queued Telegram command: %s", text);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

esp_err_t telegram_bot_poll(telegram_poll_result_t *out_result)
{
    if (!config_manager_telegram_is_configured()) {
        if (out_result) {
            *out_result = TELEGRAM_POLL_NOT_CONFIGURED;
        }
        return ESP_ERR_INVALID_STATE;
    }

    // Ensure the download directory exists (also makes it show up as a
    // regular album under IMAGE_DIRECTORY for local-storage rotation).
    mkdir(TELEGRAM_DOWNLOAD_DIRECTORY, 0775);

    int64_t offset = config_manager_get_telegram_last_update_id() + 1;
    char base_url[256];
    build_api_url("getUpdates", base_url, sizeof(base_url));
    char url[350];
    snprintf(url, sizeof(url), "%s?offset=%lld&timeout=%d&limit=%d", base_url, (long long) offset,
             TELEGRAM_POLL_TIMEOUT_SEC, TELEGRAM_MAX_UPDATES_PER_POLL);

    char *body = NULL;
    esp_err_t err = telegram_http_get(
        url, TELEGRAM_HTTP_TIMEOUT_MS + TELEGRAM_POLL_TIMEOUT_SEC * 1000, &body, NULL);
    if (err != ESP_OK || !body) {
        ESP_LOGE(TAG, "getUpdates failed: %s", esp_err_to_name(err));
        free(body);
        if (out_result) {
            *out_result = TELEGRAM_POLL_ERROR;
        }
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "getUpdates: failed to parse JSON response");
        if (out_result) {
            *out_result = TELEGRAM_POLL_ERROR;
        }
        return ESP_FAIL;
    }

    cJSON *ok_item = cJSON_GetObjectItem(root, "ok");
    cJSON *result_arr = cJSON_GetObjectItem(root, "result");
    if (!ok_item || !cJSON_IsTrue(ok_item) || !result_arr || !cJSON_IsArray(result_arr)) {
        cJSON *desc = cJSON_GetObjectItem(root, "description");
        ESP_LOGE(TAG, "getUpdates returned an error: %s",
                 (desc && cJSON_IsString(desc)) ? desc->valuestring : "unknown");
        cJSON_Delete(root);
        if (out_result) {
            *out_result = TELEGRAM_POLL_ERROR;
        }
        return ESP_FAIL;
    }

    int count = cJSON_GetArraySize(result_arr);
    if (count == 0) {
        ESP_LOGI(TAG, "No new Telegram updates");
        cJSON_Delete(root);
        if (out_result) {
            *out_result = TELEGRAM_POLL_OK;
        }
        return ESP_OK;
    }

    const char *allowed_chat_id = config_manager_get_telegram_chat_id();
    int64_t max_update_id = offset - 1;

    // ---- First pass: highest update_id in the batch + emergency reset scan ----
    bool reset_found = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, result_arr)
    {
        cJSON *uid = cJSON_GetObjectItem(item, "update_id");
        if (uid && cJSON_IsNumber(uid)) {
            int64_t id = (int64_t) uid->valuedouble;
            if (id > max_update_id) {
                max_update_id = id;
            }
        }

        cJSON *message = get_message(item);
        if (!message_from_allowed_chat(message, allowed_chat_id)) {
            continue;
        }
        const char *text = get_text(message);
        if (text && strncmp(text, TELEGRAM_RESET_COMMAND, strlen(TELEGRAM_RESET_COMMAND)) == 0) {
            reset_found = true;
        }
    }

    if (reset_found) {
        ESP_LOGW(TAG, "/telegram_reset received - discarding %d update(s), going to sleep", count);
        config_manager_set_telegram_last_update_id(max_update_id);
        cJSON_Delete(root);
        telegram_bot_send_message("Reset ausgefuehrt, Warteschlange geloescht.");
        if (out_result) {
            *out_result = TELEGRAM_POLL_RESET;
        }
        return ESP_OK;
    }

    // ---- Second pass: download images (newest wins for display), queue commands ----
    char latest_image_path[320] = {0};
    bool have_image = false;
    bool image_attempt_failed = false;

    cJSON_ArrayForEach(item, result_arr)
    {
        cJSON *message = get_message(item);
        if (!message_from_allowed_chat(message, allowed_chat_id)) {
            ESP_LOGW(TAG, "Ignoring Telegram update from a disallowed chat");
            continue;
        }

        cJSON *photo = cJSON_GetObjectItem(message, "photo");
        cJSON *document = cJSON_GetObjectItem(message, "document");
        char downloaded_path[320];
        bool got_image = false;

        if (photo && cJSON_IsArray(photo) && cJSON_GetArraySize(photo) > 0) {
            got_image =
                (download_photo_with_fallback(photo, downloaded_path, sizeof(downloaded_path)) ==
                 ESP_OK);
            if (!got_image) {
                image_attempt_failed = true;
            }
        } else if (document && cJSON_IsObject(document)) {
            got_image =
                (download_document_image(document, downloaded_path, sizeof(downloaded_path)) ==
                 ESP_OK);
            if (!got_image) {
                image_attempt_failed = true;
            }
        }

        if (got_image) {
            strncpy(latest_image_path, downloaded_path, sizeof(latest_image_path) - 1);
            latest_image_path[sizeof(latest_image_path) - 1] = '\0';
            have_image = true;
            ESP_LOGI(TAG, "Saved Telegram image: %s", downloaded_path);
        }

        const char *text = get_text(message);
        if (text && text[0] == '/') {
            queue_command(text);
        }
    }

    cJSON_Delete(root);

    if (have_image) {
        esp_err_t disp_err = process_and_display_telegram_image(latest_image_path);
        const char *fname = strrchr(latest_image_path, '/');
        fname = fname ? fname + 1 : latest_image_path;
        char msg[192];
        if (disp_err == ESP_OK) {
            snprintf(msg, sizeof(msg), "Bild empfangen und angezeigt: %s", fname);
        } else {
            snprintf(msg, sizeof(msg), "Bild empfangen (%s), aber Anzeige fehlgeschlagen: %s", fname,
                     esp_err_to_name(disp_err));
        }
        telegram_bot_send_message(msg);
    } else if (image_attempt_failed) {
        telegram_bot_send_message(
            "Telegram-Bild konnte nicht geladen werden (zu gross, nicht unterstuetztes Format, "
            "oder nur als progressives JPEG verfuegbar). Bitte kleineres Bild oder als Datei "
            "senden.");
    }

    config_manager_set_telegram_last_update_id(max_update_id);

    if (out_result) {
        *out_result = TELEGRAM_POLL_OK;
    }
    return ESP_OK;
}

// ----------------------------------------------------------------------------
// Command execution
// ----------------------------------------------------------------------------

static void build_status_message(char *out, size_t out_len)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    int battery_percent = board_hal_get_battery_percent();
    int battery_mv = board_hal_get_battery_voltage();
    bool charging = board_hal_is_charging();
    bool usb = board_hal_is_usb_connected();

    char ip_str[16] = "n/a";
    wifi_manager_get_ip(ip_str, sizeof(ip_str));

    snprintf(out, out_len,
             "PhotoFrame Status\n"
             "Firmware: %s (%s)\n"
             "Batterie: %d%% (%d mV)%s%s\n"
             "WLAN: %s\n"
             "Freier Heap: %lu Bytes",
             app_desc->version, BOARD_HAL_NAME, battery_percent, battery_mv,
             charging ? ", laedt" : "", usb ? ", USB verbunden" : "", ip_str,
             (unsigned long) esp_get_free_heap_size());
}

// Executes one queued "/"-command and sends a sendMessage reply. Strips an
// optional "@BotName" suffix (Telegram appends it in group chats).
static void execute_command(const char *raw_text)
{
    char cmd[TELEGRAM_COMMAND_MAX_LEN];
    strncpy(cmd, raw_text, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    // Isolate just the command token (up to whitespace or '@').
    char *space = strpbrk(cmd, " \t\n@");
    if (space) {
        *space = '\0';
    }

    ESP_LOGI(TAG, "Executing Telegram command: %s", cmd);

    if (strcmp(cmd, "/status") == 0) {
        char msg[256];
        build_status_message(msg, sizeof(msg));
        telegram_bot_send_message(msg);
    } else if (strcmp(cmd, "/clear") == 0) {
        esp_err_t err = display_manager_clear();
        telegram_bot_send_message(err == ESP_OK ? "Anzeige geloescht."
                                                 : "Anzeige loeschen fehlgeschlagen.");
    } else if (strcmp(cmd, "/restart") == 0) {
        telegram_bot_send_message("Neustart wird durchgefuehrt...");
        vTaskDelay(pdMS_TO_TICKS(500));  // give the HTTP send a moment to flush
        esp_restart();
        // Does not return.
    } else {
        char msg[192];
        snprintf(msg, sizeof(msg), "Unbekannter Befehl: %s\nVerfuegbar: /status, /restart, /clear",
                 cmd);
        telegram_bot_send_message(msg);
    }
}

void telegram_bot_run_pending_commands(void)
{
    for (int i = 0; i < s_pending_command_count; i++) {
        execute_command(s_pending_commands[i]);
    }
    s_pending_command_count = 0;
}
