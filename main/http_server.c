#include "http_server.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "agenda_color_profile.h"
#include "alarm_manager.h"
#include "album_manager.h"
#include "battery_history.h"
#include "board_hal.h"
#include "cJSON.h"
#include "climate_history.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "debug_log.h"
#include "display_flow.h"
#include "display_manager.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "ha_integration.h"
#include "history_manager.h"
#include "http_auth.h"
#include "https_cert.h"
#include "image_processor.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "ota_manager.h"
#include "overlay_manager.h"
#include "periodic_tasks.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "sdcard.h"
#include "storage.h"
#include "utils.h"
#include "wifi_manager.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char *TAG = "http_server";
static httpd_handle_t server = NULL;
static httpd_handle_t https_server = NULL;  // NULL unless config_manager_get_https_enabled()
static bool system_ready = false;

#define HTTPD_503 "503 Service Unavailable"

/**
 * @brief Validate a user-supplied path component to prevent directory traversal attacks.
 *
 * Rejects paths containing ".." sequences or starting with "/".
 *
 * @return true if the path is safe, false otherwise.
 */
static bool is_path_safe(const char *path)
{
    if (!path || path[0] == '/') {
        return false;
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }
    return true;
}

extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_gz_end");
extern const uint8_t index_css_start[] asm("_binary_index_css_gz_start");
extern const uint8_t index_css_end[] asm("_binary_index_css_gz_end");
extern const uint8_t index_js_start[] asm("_binary_index_js_gz_start");
extern const uint8_t index_js_end[] asm("_binary_index_js_gz_end");
extern const uint8_t index2_js_start[] asm("_binary_index2_js_gz_start");
extern const uint8_t index2_js_end[] asm("_binary_index2_js_gz_end");
extern const uint8_t exif_reader_js_start[] asm("_binary_exif_reader_js_gz_start");
extern const uint8_t exif_reader_js_end[] asm("_binary_exif_reader_js_gz_end");
extern const uint8_t browser_js_start[] asm("_binary_browser_js_gz_start");
extern const uint8_t browser_js_end[] asm("_binary_browser_js_gz_end");
extern const uint8_t vite_browser_external_js_start[] asm(
    "_binary___vite_browser_external_js_gz_start");
extern const uint8_t vite_browser_external_js_end[] asm(
    "_binary___vite_browser_external_js_gz_end");
extern const uint8_t icon_svg_start[] asm("_binary_icon_svg_gz_start");
extern const uint8_t icon_svg_end[] asm("_binary_icon_svg_gz_end");
extern const uint8_t profile_editor_html_start[] asm("_binary_profile_editor_html_gz_start");
extern const uint8_t profile_editor_html_end[] asm("_binary_profile_editor_html_gz_end");
extern const uint8_t measurement_sample_jpg_start[] asm("_binary_measurement_sample_jpg_start");
extern const uint8_t measurement_sample_jpg_end[] asm("_binary_measurement_sample_jpg_end");

// --- Optional HTTP API authentication (#130) ---
//
// Off by default: config_manager_get_http_password() returns "" unless the
// owner sets one, and most frames sit on a trusted home network. When a
// password is set, every route registered through register_uri() is gated,
// so a new endpoint is protected by construction rather than by remembering
// to add a check.
//
// HTTP Basic, deliberately: browsers prompt for it natively, so this needs no
// login page, session or cookie in the webapp. The username is ignored; the
// password is the whole credential.
//
// This is not confidential over plain HTTP -- the credential is base64, not
// encrypted, and a passive listener on the same network can replay it. It
// raises the bar against casual access on a shared LAN; it is not a defence
// against an attacker who can already sniff your traffic. Serving TLS from the
// device was considered and rejected (cert trust, RAM, battery).
//
// The captive-portal provisioning server in wifi_provisioning.c is a separate
// httpd instance and is intentionally not gated -- there is nothing to
// authenticate against before the device has been configured.

typedef esp_err_t (*http_handler_fn)(httpd_req_t *);

// Client address for the brute-force limiter, as 16 bytes (IPv4 is stored
// v4-mapped). Falls back to all-zero, i.e. one shared bucket, if the peer
// can't be read.
static void client_addr(httpd_req_t *req, uint8_t out[HTTP_AUTH_ADDR_LEN])
{
    memset(out, 0, HTTP_AUTH_ADDR_LEN);
    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *) &peer, &len) != 0) {
        return;
    }
    if (peer.ss_family == AF_INET6) {
        memcpy(out, &((struct sockaddr_in6 *) &peer)->sin6_addr, HTTP_AUTH_ADDR_LEN);
    } else if (peer.ss_family == AF_INET) {
        out[10] = 0xff;
        out[11] = 0xff;
        memcpy(out + 12, &((struct sockaddr_in *) &peer)->sin_addr, 4);
    }
}

typedef enum { AUTH_OK, AUTH_DENIED, AUTH_LOCKED_OUT } auth_result_t;

static auth_result_t http_auth_check(httpd_req_t *req, int64_t *retry_after_ms)
{
    const char *expected = config_manager_get_http_password();
    if (expected == NULL || expected[0] == '\0') {
        return AUTH_OK;  // authentication disabled
    }

    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0) {
        // No credential offered: the browser's first request before it shows
        // its password prompt. Not a guess, so it doesn't count.
        return AUTH_DENIED;
    }

    uint8_t addr[HTTP_AUTH_ADDR_LEN];
    client_addr(req, addr);
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (!http_auth_limiter_allowed(addr, now_ms, retry_after_ms)) {
        return AUTH_LOCKED_OUT;
    }

    bool ok = false;
    char header[257];
    if (len <= 256 &&
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK) {
        ok = http_auth_header_matches(header, expected);
    }
    http_auth_limiter_record(addr, ok, now_ms);
    if (!ok) {
        ESP_LOGW(TAG, "Wrong device password from a client");
    }
    return ok ? AUTH_OK : AUTH_DENIED;
}

// Dispatch trampoline: the real handler travels in user_ctx (no route used it).
static esp_err_t auth_gate(httpd_req_t *req)
{
    int64_t retry_after_ms = 0;
    auth_result_t result = http_auth_check(req, &retry_after_ms);
    if (result == AUTH_LOCKED_OUT) {
        // Lockouts are capped at HTTP_AUTH_LOCKOUT_MAX_MS, so this fits.
        char retry_after[12];
        snprintf(retry_after, sizeof(retry_after), "%u",
                 (unsigned) ((retry_after_ms + 999) / 1000));
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_hdr(req, "Retry-After", retry_after);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"too many wrong passwords, try again later\"}");
        return ESP_OK;
    }
    if (result == AUTH_DENIED) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 PhotoFrame\"");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"authentication required\"}");
        return ESP_OK;
    }
    return ((http_handler_fn) req->user_ctx)(req);
}

// Register a route behind the optional auth gate. The real handler rides in
// user_ctx; every route goes through here so authentication cannot be
// forgotten when a new endpoint is added. Takes an explicit handle so the
// same registration list can be replayed onto both the plain-HTTP server and
// the optional HTTPS one (see register_all_handlers()).
static void register_uri(httpd_handle_t handle, const char *uri, httpd_method_t method,
                         http_handler_fn handler)
{
    httpd_uri_t u = {
        .uri = uri, .method = method, .handler = auth_gate, .user_ctx = (void *) handler};
    httpd_register_uri_handler(handle, &u);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) index_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t index_css_handler(httpd_req_t *req)
{
    const size_t index_css_size = (index_css_end - index_css_start);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) index_css_start, index_css_size);
    return ESP_OK;
}

static esp_err_t index_js_handler(httpd_req_t *req)
{
    const size_t index_js_size = (index_js_end - index_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) index_js_start, index_js_size);
    return ESP_OK;
}

static esp_err_t index2_js_handler(httpd_req_t *req)
{
    const size_t index2_js_size = (index2_js_end - index2_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) index2_js_start, index2_js_size);
    return ESP_OK;
}

static esp_err_t exif_reader_js_handler(httpd_req_t *req)
{
    const size_t exif_reader_js_size = (exif_reader_js_end - exif_reader_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) exif_reader_js_start, exif_reader_js_size);
    return ESP_OK;
}

static esp_err_t browser_js_handler(httpd_req_t *req)
{
    const size_t browser_js_size = (browser_js_end - browser_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) browser_js_start, browser_js_size);
    return ESP_OK;
}

static esp_err_t vite_browser_external_js_handler(httpd_req_t *req)
{
    const size_t vite_browser_external_js_size =
        (vite_browser_external_js_end - vite_browser_external_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) vite_browser_external_js_start,
                    vite_browser_external_js_size);
    return ESP_OK;
}

static esp_err_t icon_handler(httpd_req_t *req)
{
    const size_t icon_svg_size = (icon_svg_end - icon_svg_start);
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) icon_svg_start, icon_svg_size);
    return ESP_OK;
}

// The standalone Calendar color-profile visual editor tool (profile-editor.html,
// webapp/public/) - served by the device itself so its "An Gerät senden" button
// (a same-origin fetch to POST /api/agenda/color-profile?slot=N) has a device to
// talk to without the user needing to download/re-upload the exported JSON by
// hand. Embedded the same way as index.html/icon.svg above.
static esp_err_t profile_editor_handler(httpd_req_t *req)
{
    const size_t profile_editor_html_size = (profile_editor_html_end - profile_editor_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) profile_editor_html_start, profile_editor_html_size);
    return ESP_OK;
}

static esp_err_t measurement_sample_handler(httpd_req_t *req)
{
    const size_t measurement_sample_jpg_size =
        (measurement_sample_jpg_end - measurement_sample_jpg_start);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, (const char *) measurement_sample_jpg_start, measurement_sample_jpg_size);
    return ESP_OK;
}

// Shared multipart parsing helper
typedef struct {
    char image_path[512];
    char thumbnail_path[512];
    char original_filename[256];  // Original filename from upload
    bool has_image;
    bool has_thumbnail;
} multipart_result_t;

static esp_err_t parse_multipart_upload(httpd_req_t *req, const char *base_dir,
                                        const char *image_filename, const char *thumb_filename,
                                        multipart_result_t *result, bool require_png)
{
    result->has_image = false;
    result->has_thumbnail = false;

    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Processing multipart upload, content length: %d", req->content_len);

    int buf_len = 0;
    int remaining = req->content_len;

    char boundary[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", boundary, sizeof(boundary)) != ESP_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No Content-Type header");
        return ESP_FAIL;
    }

    char *boundary_start = strstr(boundary, "boundary=");
    if (!boundary_start) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary found");
        return ESP_FAIL;
    }

    boundary_start += 9;
    char *boundary_end = strchr(boundary_start, '\r');
    if (!boundary_end)
        boundary_end = strchr(boundary_start, '\n');
    if (!boundary_end)
        boundary_end = strchr(boundary_start, ';');
    if (!boundary_end)
        boundary_end = boundary_start + strlen(boundary_start);

    int boundary_value_len = boundary_end - boundary_start;
    snprintf(boundary, sizeof(boundary), "--%.*s", boundary_value_len, boundary_start);
    int full_boundary_len = strlen(boundary);

    char current_field[64] = {0};
    bool header_parsed = false;
    FILE *fp = NULL;

    while (remaining > 0 || buf_len > 0) {
        if (remaining > 0 && buf_len < 2048) {
            int to_read = MIN(remaining, 4096 - buf_len);
            int received = httpd_req_recv(req, buf + buf_len, to_read);

            if (received <= 0) {
                if (received == HTTPD_SOCK_ERR_TIMEOUT)
                    continue;
                if (fp)
                    fclose(fp);
                free(buf);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
                return ESP_FAIL;
            }

            buf_len += received;
            remaining -= received;
        }

        if (!header_parsed) {
            char *name_start = strstr(buf, "name=\"");
            if (name_start && name_start < buf + buf_len) {
                name_start += 6;
                char *name_end = strchr(name_start, '"');
                if (name_end && name_end < buf + buf_len) {
                    int name_len = name_end - name_start;
                    strncpy(current_field, name_start, MIN(name_len, sizeof(current_field) - 1));
                    current_field[MIN(name_len, sizeof(current_field) - 1)] = '\0';
                }
            }

            char *filename_start = strstr(buf, "filename=\"");
            if (filename_start && filename_start < buf + buf_len) {
                filename_start += 10;
                char *filename_end = strchr(filename_start, '"');
                if (filename_end && filename_end < buf + buf_len) {
                    int name_len = filename_end - filename_start;

                    if (strcmp(current_field, "image") == 0) {
                        // Capture original filename
                        strncpy(result->original_filename, filename_start,
                                MIN(name_len, sizeof(result->original_filename) - 1));
                        result->original_filename[MIN(
                            name_len, sizeof(result->original_filename) - 1)] = '\0';

                        if (require_png) {
                            // Check PNG extension for image field
                            char *ext = strrchr(result->original_filename, '.');
                            if (!ext ||
                                (strcasecmp(ext, ".png") != 0 && strcasecmp(ext, ".epdgz") != 0)) {
                                if (fp)
                                    fclose(fp);
                                free(buf);
                                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                                    "Only PNG and EPDGZ files are allowed");
                                return ESP_FAIL;
                            }
                        }

                        snprintf(result->image_path, sizeof(result->image_path), "%s/%s", base_dir,
                                 image_filename);
                        fp = fopen(result->image_path, "wb");
                        if (!fp) {
                            free(buf);
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                "Failed to create file");
                            return ESP_FAIL;
                        }
                        result->has_image = true;
                    } else if (strcmp(current_field, "thumbnail") == 0) {
                        snprintf(result->thumbnail_path, sizeof(result->thumbnail_path), "%s/%s",
                                 base_dir, thumb_filename);
                        fp = fopen(result->thumbnail_path, "wb");
                        if (!fp) {
                            free(buf);
                            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                "Failed to create file");
                            return ESP_FAIL;
                        }
                        result->has_thumbnail = true;
                    }
                }
            }

            char *data_start = strstr(buf, "\r\n\r\n");
            if (data_start && data_start < buf + buf_len) {
                data_start += 4;
                int header_len = data_start - buf;
                header_parsed = true;
                buf_len -= header_len;
                memmove(buf, data_start, buf_len);
            } else if (remaining == 0) {
                break;
            }
        } else {
            char *boundary_pos = NULL;
            for (int i = 0; i <= buf_len - full_boundary_len; i++) {
                if (memcmp(buf + i, boundary, full_boundary_len) == 0) {
                    boundary_pos = buf + i;
                    break;
                }
            }

            if (boundary_pos) {
                int data_len = boundary_pos - buf;
                if (fp && data_len > 0) {
                    fwrite(buf, 1, data_len, fp);
                }

                if (fp) {
                    fclose(fp);
                    fp = NULL;
                }

                int consumed = (boundary_pos - buf) + full_boundary_len;
                buf_len -= consumed;
                memmove(buf, buf + consumed, buf_len);
                header_parsed = false;
                current_field[0] = '\0';
            } else {
                int safe_write = buf_len - (full_boundary_len - 1);
                if (safe_write > 0 && remaining > 0) {
                    if (fp) {
                        fwrite(buf, 1, safe_write, fp);
                    }
                    buf_len -= safe_write;
                    memmove(buf, buf + safe_write, buf_len);
                } else if (remaining == 0 && buf_len > 0) {
                    if (fp) {
                        fwrite(buf, 1, buf_len, fp);
                    }
                    buf_len = 0;
                }
            }
        }
    }

    if (fp) {
        fclose(fp);
    }
    free(buf);

    return ESP_OK;
}

// Read a whole file into a PSRAM buffer (caller frees with heap_caps_free)
// ---------- Direct display flow ----------

static esp_err_t send_display_success(httpd_req_t *req)
{
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "message", "Image displayed successfully");
    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send response (connection likely closed): %d", send_err);
    }
    return ESP_OK;
}

// Map a processing failure to the right HTTP error, with detail when the
// image processor recorded any
static void send_process_error(httpd_req_t *req, esp_err_t err)
{
    ESP_LOGE(TAG, "Failed to process image: %s", esp_err_to_name(err));
    if (err == ESP_ERR_INVALID_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Image is too large. Please resize and try again.");
    } else if (err == ESP_ERR_NO_MEM) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Image requires too much memory to process.");
    } else {
        const char *detail = image_processor_get_last_error();
        char errmsg[160];
        snprintf(errmsg, sizeof(errmsg), "Failed to process image%s%s", detail[0] ? ": " : "",
                 detail);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, errmsg);
    }
}

// Common tail of both direct display arms once the source image (and an
// optional thumbnail) sit in files: put the image on the panel, retire the
// original into the .current.* scheme, and answer the request. Consumes
// both files.
static esp_err_t display_received_image(httpd_req_t *req, const char *image_path,
                                        image_format_t format, const char *thumbnail_path)
{
    bool has_thumbnail = thumbnail_path != NULL;

    if (format == IMAGE_FORMAT_EPD_GZ || format == IMAGE_FORMAT_BMP) {
        // Display-ready file formats: move into the .current.* slot and let
        // display_manager decode from there
        const char *display_path = display_flow_stage_file(image_path, format);
        if (!display_path) {
            if (has_thumbnail)
                unlink(thumbnail_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                format == IMAGE_FORMAT_EPD_GZ ? "Failed to process EPDGZ"
                                                              : "Failed to process BMP");
            return ESP_FAIL;
        }

        if (display_manager_show_image(display_path) != ESP_OK) {
            // Drop the staged file: a previous display's link may point at
            // this .current.* name, and it must not resolve to the failed
            // upload
            unlink(display_path);
            if (has_thumbnail)
                unlink(thumbnail_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to display image");
            return ESP_FAIL;
        }

        // Stage the provided thumbnail only after a successful display so a
        // failed request can't clobber the previous image's thumbnail
        if (has_thumbnail) {
            unlink(CURRENT_JPG_PATH);
            if (rename(thumbnail_path, CURRENT_JPG_PATH) != 0) {
                ESP_LOGW(TAG, "Failed to save thumbnail");
                unlink(thumbnail_path);
                has_thumbnail = false;
            }
        }

        display_flow_drop_stale_current(display_path, has_thumbnail);
        // EPDGZ is not servable by /api/current_image and is deleted as
        // before
        unlink(CURRENT_EPD_PATH);
    } else {
        // Raw PNG or JPG: process and stream rows straight to the display --
        // no rendered-file round-trip. /api/current_image tries the .jpg
        // sibling first, then falls back to the published name with its
        // native content type.
        display_publish_t pub = {.display_name = (format == IMAGE_FORMAT_JPG) ? CURRENT_JPG_PATH
                                                                              : CURRENT_PNG_PATH};

        esp_err_t err = display_flow_stream_file(image_path, format,
                                                 processing_settings_get_dithering_algorithm(),
                                                 &pub, !storage_has_persistent_storage());
        if (err != ESP_OK) {
            unlink(image_path);
            if (has_thumbnail)
                unlink(thumbnail_path);
            send_process_error(req, err);
            return ESP_FAIL;
        }

        // The device cannot encode JPEG, so the displayed original becomes
        // the /api/current_image source; a provided thumbnail wins over a
        // JPG original
        display_flow_retire_source(image_path, format, has_thumbnail);
        if (has_thumbnail) {
            unlink(CURRENT_JPG_PATH);
            if (rename(thumbnail_path, CURRENT_JPG_PATH) != 0) {
                ESP_LOGW(TAG, "Failed to save thumbnail");
                unlink(thumbnail_path);
            }
        }
    }

    ha_notify_update();
    return send_display_success(req);
}

// Receive a raw request body into dest_path (the error response is sent
// here on failure)
static esp_err_t receive_raw_body(httpd_req_t *req, const char *dest_path)
{
    size_t content_len = req->content_len;
    const size_t MAX_UPLOAD_SIZE = 5 * 1024 * 1024;  // 5MB max

    if (content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
        return ESP_FAIL;
    }

    if (content_len > MAX_UPLOAD_SIZE) {
        ESP_LOGW(TAG, "Upload rejected: %zu bytes exceeds limit of %zu bytes", content_len,
                 MAX_UPLOAD_SIZE);
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg),
                 "File too large: %zu KB (max: %zu KB). Please compress or resize your image.",
                 content_len / 1024, MAX_UPLOAD_SIZE / 1024);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error_msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving image for direct display, size: %zu bytes (%.1f KB)", content_len,
             content_len / 1024.0);

    // Only the upload temp is cleared up front; the current-image files are
    // replaced on success so a failed upload can't break /api/current_image
    unlink(dest_path);

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to create temporary file");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to create temporary file");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < content_len) {
        size_t to_read = MIN(4096, content_len - received);
        int ret = httpd_req_recv(req, buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive data");
            free(buf);
            fclose(fp);
            unlink(dest_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_FAIL;
        }

        fwrite(buf, 1, ret, fp);
        received += ret;
    }

    free(buf);
    fclose(fp);

    ESP_LOGI(TAG, "Image received successfully");
    return ESP_OK;
}

static esp_err_t display_image_direct_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    // Check if display is already busy
    if (display_manager_is_busy()) {
        ESP_LOGW(TAG, "Display is busy, rejecting request");
        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "busy");
        cJSON_AddStringToObject(response, "message", "Display is currently updating, please wait");

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    // Get content type to determine if it's JPG, BMP, PNG, or multipart
    char content_type[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) !=
        ESP_OK) {
        strcpy(content_type, "image/jpeg");  // Default to JPEG
    }

    if (strstr(content_type, "multipart/form-data") != NULL) {
        // Multipart upload with an optional pre-rendered thumbnail
        multipart_result_t result;
        esp_err_t err = parse_multipart_upload(req, FS_MOUNT_POINT, ".current_upload.tmp",
                                               ".current_thumb.tmp", &result, false);
        if (err != ESP_OK) {
            return ESP_FAIL;
        }

        if (!result.has_image) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No image file in multipart upload");
            return ESP_FAIL;
        }

        image_format_t format = image_processor_detect_format(result.image_path);
        if (format == IMAGE_FORMAT_UNKNOWN) {
            unlink(result.image_path);
            if (result.has_thumbnail)
                unlink(result.thumbnail_path);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported image format");
            return ESP_FAIL;
        }

        return display_received_image(req, result.image_path, format,
                                      result.has_thumbnail ? result.thumbnail_path : NULL);
    }

    // Raw body upload: the format comes from the content type, or from the
    // file magic when the header is missing/generic
    image_format_t format = IMAGE_FORMAT_UNKNOWN;
    if (strstr(content_type, "image/png")) {
        format = IMAGE_FORMAT_PNG;
    } else if (strstr(content_type, "image/bmp")) {
        format = IMAGE_FORMAT_BMP;
    } else if (strstr(content_type, "image/jpeg")) {
        format = IMAGE_FORMAT_JPG;
    }

    if (receive_raw_body(req, CURRENT_UPLOAD_PATH) != ESP_OK) {
        return ESP_FAIL;
    }

    if (format == IMAGE_FORMAT_UNKNOWN) {
        format = image_processor_detect_format(CURRENT_UPLOAD_PATH);
        if (format == IMAGE_FORMAT_UNKNOWN) {
            ESP_LOGE(TAG, "Unsupported image format or format detection failed");
            unlink(CURRENT_UPLOAD_PATH);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported image format");
            return ESP_FAIL;
        }
    }

    return display_received_image(req, CURRENT_UPLOAD_PATH, format, NULL);
}

// URL decode helper function to handle encoded characters like %20 for space
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dst_size - 1) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            // Convert hex to char
            char hex[3] = {src[i + 1], src[i + 2], '\0'};
            dst[j++] = (char) strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            // '+' is also used for space in query strings
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static esp_err_t upload_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    ESP_LOGI(TAG, "Upload started, content length: %d", req->content_len);

    // Get album name from query parameter, default to "Default"
    char album_name[128] = DEFAULT_ALBUM_NAME;
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char album_param[128];
        if (httpd_query_key_value(query, "album", album_param, sizeof(album_param)) == ESP_OK) {
            // URL decode the album name to handle special characters like '+'
            url_decode(album_name, album_param, sizeof(album_name));
        }
    }

    ESP_LOGI(TAG, "Uploading to album: %s", album_name);
    char album_path[256];

    // Get album path and ensure directory exists
    album_manager_get_album_path(album_name, album_path, sizeof(album_path));
    struct stat st;
    if (stat(album_path, &st) != 0) {
        ESP_LOGI(TAG, "Creating album directory: %s", album_path);
        if (mkdir(album_path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create directory: %s, errno: %d", album_path, errno);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Failed to create album directory");
            return ESP_FAIL;
        }
    }

    // Use shared multipart parser
    multipart_result_t result;
    esp_err_t err = parse_multipart_upload(req, album_path, "temp_full.png", "temp_thumb.jpg",
                                           &result, true);  // require_png = true
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    if (!result.has_image || !result.has_thumbnail) {
        if (result.has_image)
            unlink(result.image_path);
        if (result.has_thumbnail)
            unlink(result.thumbnail_path);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Upload incomplete - expected image and thumbnail");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Upload complete, saving PNG directly");

    // Use original filename from upload
    char filename_base[120];
    char *ext = strrchr(result.original_filename, '.');
    if (ext) {
        int base_len = ext - result.original_filename;
        int safe_len = MIN(base_len, (int) sizeof(filename_base) - 1);
        snprintf(filename_base, sizeof(filename_base), "%.*s", safe_len, result.original_filename);
    } else {
        // Copy up to buffer size - 1 to ensure null termination
        strncpy(filename_base, result.original_filename, sizeof(filename_base) - 1);
        filename_base[sizeof(filename_base) - 1] = '\0';
    }

    char file_ext[16] = ".png";
    if (ext && strcasecmp(ext, ".epdgz") == 0) {
        strcpy(file_ext, ".epdgz");
    }

    char dest_filename[256];
    char jpg_filename[256];
    char final_dest_path[512];
    char final_thumb_path[512];

    // Use original filename (will overwrite if exists)
    snprintf(dest_filename, sizeof(dest_filename), "%s%s", filename_base, file_ext);
    snprintf(jpg_filename, sizeof(jpg_filename), "%s.jpg", filename_base);
    snprintf(final_dest_path, sizeof(final_dest_path), "%s/%s", album_path, dest_filename);
    snprintf(final_thumb_path, sizeof(final_thumb_path), "%s/%s", album_path, jpg_filename);

    // Remove old files
    unlink(final_dest_path);
    unlink(final_thumb_path);

    // Move PNG/EPDGZ to final location
    ESP_LOGI(TAG, "Saving image: %s -> %s", result.image_path, final_dest_path);
    if (rename(result.image_path, final_dest_path) != 0) {
        ESP_LOGE(TAG, "Failed to move image to album");
        unlink(result.image_path);
        unlink(result.thumbnail_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save image");
        return ESP_FAIL;
    }

    // Move thumbnail to final location
    if (rename(result.thumbnail_path, final_thumb_path) != 0) {
        ESP_LOGW(TAG, "Failed to move thumbnail");
        unlink(result.thumbnail_path);
    }

    ESP_LOGI(TAG, "Image saved successfully: %s (thumbnail: %s)", dest_filename, jpg_filename);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "filepath", final_dest_path);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}
static esp_err_t serve_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    // Get filename from query parameter
    char filename[128];
    size_t buf_len = sizeof(filename);

    if (httpd_req_get_url_query_str(req, filename, buf_len) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename specified");
        return ESP_FAIL;
    }

    // Extract 'filepath' parameter value (album/filename format)
    char param_value[128];
    if (httpd_query_key_value(filename, "filepath", param_value, sizeof(param_value)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filepath parameter");
        return ESP_FAIL;
    }

    // URL decode the filename to handle spaces and special characters
    char decoded_filename[256];
    url_decode(decoded_filename, param_value, sizeof(decoded_filename));

    if (!is_path_safe(decoded_filename)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filepath");
        return ESP_FAIL;
    }

    char filepath[512];
    const char *content_type = "image/jpeg";

    // Filename can be "album/file.jpg" or just "file.jpg"
    // Build full path
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, decoded_filename);

    // Detect content type from extension
    char *ext = strrchr(decoded_filename, '.');
    if (ext) {
        if (strcasecmp(ext, ".png") == 0) {
            content_type = "image/png";
        } else if (strcasecmp(ext, ".bmp") == 0) {
            content_type = "image/bmp";
        }
    }

    FILE *fp = fopen(filepath, "rb");

    // If JPG doesn't exist and request was for .jpg, try .png then .bmp fallback
    if (!fp) {
        if (ext && strcasecmp(ext, ".jpg") == 0) {
            // Try .png fallback first
            char png_filename[256];
            strncpy(png_filename, decoded_filename, sizeof(png_filename) - 1);
            char *png_ext = strrchr(png_filename, '.');
            if (png_ext) {
                strcpy(png_ext, ".png");
            }

            snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, png_filename);
            fp = fopen(filepath, "rb");
            if (fp) {
                content_type = "image/png";
                ESP_LOGW(TAG, "JPG thumbnail not found, serving PNG: %s", png_filename);
            } else {
                // Try .bmp fallback
                char bmp_filename[256];
                strncpy(bmp_filename, decoded_filename, sizeof(bmp_filename) - 1);
                char *bmp_ext = strrchr(bmp_filename, '.');
                if (bmp_ext) {
                    strcpy(bmp_ext, ".bmp");
                }

                snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, bmp_filename);
                fp = fopen(filepath, "rb");
                if (fp) {
                    content_type = "image/bmp";
                    ESP_LOGW(TAG, "JPG thumbnail not found, serving BMP: %s", bmp_filename);
                }
            }
        }
    }

    if (!fp) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Image not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type);
    // Cache images for 1 hour to reduce server load
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");

    // 4KB rather than 1KB: this handler is hit once per gallery thumbnail, and
    // with many concurrent requests (a large album with thumbnails enabled)
    // fewer, bigger chunks means each connection ties up the single httpd
    // task for less time, freeing sockets for the next request sooner.
    char buffer[4096];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
    }

    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t delete_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }
    if (!storage_has_persistent_storage()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage not found");
        return ESP_FAIL;
    }

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *filepath_obj = cJSON_GetObjectItem(root, "filepath");
    if (!filepath_obj || !cJSON_IsString(filepath_obj)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filepath");
        return ESP_FAIL;
    }

    const char *filepath_str = filepath_obj->valuestring;

    if (!is_path_safe(filepath_str)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filepath");
        return ESP_FAIL;
    }

    // Copy filepath to local buffer before deleting JSON
    char filepath_copy[256];
    strncpy(filepath_copy, filepath_str, sizeof(filepath_copy) - 1);
    filepath_copy[sizeof(filepath_copy) - 1] = '\0';

    // Build full path - filepath is "album/file.bmp" format
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, filepath_copy);

    // Also delete the corresponding JPEG thumbnail
    char jpg_filename[256];
    strncpy(jpg_filename, filepath_copy, sizeof(jpg_filename) - 1);
    jpg_filename[sizeof(jpg_filename) - 1] = '\0';
    char *ext = strrchr(jpg_filename, '.');
    if (ext && (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
                strcasecmp(ext, ".epdgz") == 0)) {
        strcpy(ext, ".jpg");
    }

    char jpg_path[512];
    snprintf(jpg_path, sizeof(jpg_path), "%s/%s", IMAGE_DIRECTORY, jpg_filename);

    // Delete JSON before file operations
    cJSON_Delete(root);

    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }

    // Delete thumbnail (ignore errors if it doesn't exist)
    unlink(jpg_path);

    ESP_LOGI(TAG, "Image deleted successfully: %s", filepath_copy);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

static esp_err_t display_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    // Check if display is already busy
    if (display_manager_is_busy()) {
        ESP_LOGW(TAG, "Display is busy, rejecting request");
        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "busy");
        cJSON_AddStringToObject(response, "message", "Display is currently updating, please wait");

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *filepath_obj = cJSON_GetObjectItem(root, "filepath");
    if (!filepath_obj || !cJSON_IsString(filepath_obj)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filepath");
        return ESP_FAIL;
    }

    const char *filepath_str = filepath_obj->valuestring;

    if (!is_path_safe(filepath_str)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filepath");
        return ESP_FAIL;
    }

    // Build absolute path - filepath is "album/file.bmp" format
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, filepath_str);

    // Weather/headline overlays (if enabled) apply here too, same as the
    // Auto-Rotate loops - previously this direct-display action bypassed
    // overlay_manager_apply() entirely, so neither overlay ever showed up
    // regardless of format or settings.
    const char *shown = overlay_manager_apply(filepath);
    esp_err_t err = display_manager_show_image(shown);
    if (err == ESP_OK && strcmp(shown, filepath) != 0) {
        // The overlay was drawn onto a scratch copy - display_manager_show_image()
        // already marked *that* path as shown; re-mark the real album file too,
        // same reasoning as the Auto-Rotate loops' identical correction.
        history_manager_mark_shown(filepath);
    }

    cJSON_Delete(root);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to display image");
        return ESP_FAIL;
    }

    ha_notify_update();

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");

    esp_err_t send_err = httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send response (connection likely closed): %d", send_err);
    }

    return ESP_OK;
}

static esp_err_t battery_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    cJSON *response = create_battery_json();
    if (response == NULL) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Failed to create battery JSON");
        return ESP_FAIL;
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

static esp_err_t battery_history_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_DELETE) {
        battery_history_reset();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
        return ESP_OK;
    }

    cJSON *response = battery_history_build_json();
    if (response == NULL) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Failed to build battery history JSON");
        return ESP_FAIL;
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

static esp_err_t climate_history_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_DELETE) {
        climate_history_reset();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
        return ESP_OK;
    }

    cJSON *response = climate_history_build_json();
    if (response == NULL) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Failed to build climate history JSON");
        return ESP_FAIL;
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

// GET returns how many images have been marked shown in the current
// no-repeat cycle (history_manager.h); DELETE clears it and restarts the
// cycle - same effect as the "/clear_history" Telegram command, including
// resetting the sequential-rotation cursor so both rotation modes start
// fresh, not just the random-mode history set.
static esp_err_t display_history_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_DELETE) {
        history_manager_clear();
        config_manager_set_last_index(-1);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
        return ESP_OK;
    }

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Failed to build display history JSON");
        return ESP_FAIL;
    }
    cJSON_AddNumberToObject(response, "count", history_manager_count());

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

// Web UI maintenance action for the Cover/Fit variant-selection feature
// (see docs/FACE_CROP.md) - moves every loose "<name>.cover.<ext>" across
// every album into that album's "crop" subdirectory. Global, no per-album
// parameter (applies to every album, matching the button's own "organize
// every album" scope).
static esp_err_t organize_crop_variants_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    int moved_count = 0;
    esp_err_t err = album_manager_organize_crop_variants(&moved_count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to organize crop/ folders");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddNumberToObject(response, "moved", moved_count);
    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t sensor_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Failed to create JSON response");
        return ESP_FAIL;
    }

    // Check if sensor is available
    // Check if sensor is available
    float temperature, humidity;
    bool has_temp = (board_hal_get_temperature(&temperature) == ESP_OK);
    bool has_hum = (board_hal_get_humidity(&humidity) == ESP_OK);

    if (has_temp && has_hum) {
        cJSON_AddNumberToObject(response, "temperature", temperature);
        cJSON_AddNumberToObject(response, "humidity", humidity);
        cJSON_AddStringToObject(response, "status", "ok");
    } else {
        cJSON_AddNullToObject(response, "temperature");
        cJSON_AddNullToObject(response, "humidity");
        cJSON_AddStringToObject(response, "status", "read_error");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ESP_OK;
}

static void delayed_sleep_task(void *arg)
{
    // Wait for HTTP response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Delayed sleep task: entering sleep now");
    power_manager_enter_sleep();

    // Task will be deleted when device enters deep sleep
    vTaskDelete(NULL);
}

static esp_err_t sleep_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "message", "Entering sleep mode");

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    // Create a task to enter sleep after HTTP response completes
    xTaskCreate(delayed_sleep_task, "delayed_sleep", 4096, NULL, 5, NULL);

    return ESP_OK;
}

static esp_err_t format_storage_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    storage_type_t storage_type = storage_get_type();
    if (storage_type != STORAGE_TYPE_LITTLEFS && storage_type != STORAGE_TYPE_SDCARD) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Format requires a persistent storage (SD card or flash)");
        return ESP_FAIL;
    }

    esp_err_t ret = storage_format();
    cJSON *response = cJSON_CreateObject();

    if (ret == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Storage formatted successfully");
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to format storage");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    return ret == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t rotate_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Manual rotation triggered via API");

    power_manager_reset_sleep_timer();

    // Synchronous rotation as requested by maintainer
    esp_err_t rotated = trigger_image_rotation();
    if (rotated == ESP_OK) {
        // The server answered. A timer wake whose own fetch just failed keeps
        // this server up for the HA/config window, and the backoff that
        // failure armed is stale now. A failure here adds nothing the wake
        // hasn't recorded already; on any other wake there is no backoff.
        power_manager_record_network_wake(true);
    }
    ha_notify_update();

    cJSON *response = cJSON_CreateObject();
    if (rotated == ESP_OK) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Image rotation triggered");
    } else {
        // A failed URL fetch keeps the current picture (nothing falls back to
        // a local rotation any more), so the caller must hear that nothing
        // changed rather than "success". The reason is what the fetch left in
        // last_fetch_error.
        const char *why = utils_get_last_fetch_error();
        httpd_resp_set_status(req, "502 Bad Gateway");
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message",
                                why && why[0] ? why : "Failed to fetch image from URL");
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);

    // The response above is complete either way. A handler error here would
    // only make esp_http_server close the session under a keep-alive client.
    return ESP_OK;
}

static esp_err_t current_image_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    const char *content_type = NULL;
    FILE *fp = display_flow_open_current(&content_type);
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No image currently displayed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Serving current image (%s)", content_type);

    httpd_resp_set_type(req, content_type);
    // Cache for 30 seconds since current image changes infrequently
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=30");

    char buffer[1024];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
    }

    fclose(fp);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

// Stream the debug log (previous generation first, then current) as a single
// text download.
static esp_err_t debug_log_download_handler(httpd_req_t *req)
{
    const char *paths[] = {debug_log_old_path(), debug_log_current_path()};

    struct stat st;
    bool any = false;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (stat(paths[i], &st) == 0) {
            any = true;
        }
    }
    if (!any) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No debug logs available");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"debug.log\"");

    // Hold the debug log lock so the writer task doesn't rotate the files out
    // from under us mid-stream.
    debug_log_lock();
    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && ret == ESP_OK; i++) {
        FILE *fp = fopen(paths[i], "r");
        if (!fp) {
            continue;
        }
        char buffer[1024];
        size_t read_bytes;
        while ((read_bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
            if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
                ret = ESP_FAIL;
                break;
            }
        }
        fclose(fp);
    }
    debug_log_unlock();

    if (ret != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t debug_log_clear_handler(httpd_req_t *req)
{
    debug_log_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    return ESP_OK;
}

// On-demand offline hotspot (github.com/aitjcize/esp32-photoframe#90) -
// mirrors the long-BOOT-hold trigger in main.c, for a user without physical
// access to the device. The response is sent BEFORE actually switching WiFi
// modes, since a request that arrived over the STA network the device is
// about to drop can't be answered afterward - the client needs the SSID in
// hand to reconnect via the new hotspot regardless of whether this exact
// request round-trip completes cleanly on their end.
static esp_err_t wifi_hotspot_start_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"starting\",\"ssid\":\"%s\",\"url\":\"http://192.168.4.1\"}",
             get_setup_ap_ssid());
    httpd_resp_sendstr(req, resp);
    wifi_manager_start_ap_hotspot(NULL, 0);
    return ESP_OK;
}

static esp_err_t wifi_hotspot_stop_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"stopping\"}");
    wifi_manager_stop_ap_hotspot();
    return ESP_OK;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();
        // General
        const char *device_name = config_manager_get_device_name();
        cJSON_AddStringToObject(root, "device_name", device_name ? device_name : "PhotoFrame");
        // device_id is intentionally omitted here — it's reported by
        // /api/system-info, which is what all clients read.

        const char *timezone = config_manager_get_timezone();
        cJSON_AddStringToObject(root, "timezone", timezone ? timezone : "UTC0");

        const char *ntp_server = config_manager_get_ntp_server();
        // Advanced network settings (#43): custom NTP, static IP, DNS override
        cJSON_AddStringToObject(root, "ntp_server", ntp_server ? ntp_server : DEFAULT_NTP_SERVER);
        cJSON_AddStringToObject(root, "ip_mode",
                                config_manager_get_ip_mode() == IP_MODE_STATIC ? "static" : "dhcp");
        cJSON_AddStringToObject(root, "static_ip", config_manager_get_static_ip());
        cJSON_AddStringToObject(root, "static_netmask", config_manager_get_static_netmask());
        cJSON_AddStringToObject(root, "static_gateway", config_manager_get_static_gateway());
        cJSON_AddStringToObject(root, "dns_server", config_manager_get_dns_server());

        const char *wifi_ssid = config_manager_get_wifi_ssid();
        cJSON_AddStringToObject(root, "wifi_ssid", wifi_ssid ? wifi_ssid : "");

        cJSON_AddStringToObject(
            root, "display_orientation",
            config_manager_get_display_orientation() == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape"
                                                                                      : "portrait");
        cJSON_AddNumberToObject(root, "display_rotation_deg",
                                config_manager_get_display_rotation_deg());

        // Auto Rotate
        cJSON_AddBoolToObject(root, "auto_rotate", config_manager_get_auto_rotate());
        cJSON *cron_arr = cJSON_CreateArray();
        int cron_count = config_manager_get_cron_rule_count();
        for (int i = 0; i < cron_count; i++) {
            const char *rule = config_manager_get_cron_rule(i);
            if (rule) {
                cJSON_AddItemToArray(cron_arr, cJSON_CreateString(rule));
            }
        }
        cJSON_AddItemToObject(root, "rotate_cron", cron_arr);
        const char *rotation_mode_str = "storage";
        rotation_mode_t rm = config_manager_get_rotation_mode();
        if (rm == ROTATION_MODE_URL)
            rotation_mode_str = "url";
        else if (rm == ROTATION_MODE_TELEGRAM)
            rotation_mode_str = "telegram";
        cJSON_AddStringToObject(root, "rotation_mode", rotation_mode_str);

        // Auto Rotate - SDCARD
        cJSON_AddStringToObject(root, "sd_rotation_mode",
                                config_manager_get_sd_rotation_mode() == SD_ROTATION_SEQUENTIAL
                                    ? "sequential"
                                    : "random");

        // Auto Rotate - URL
        const char *image_url = config_manager_get_image_url();
        cJSON_AddStringToObject(root, "image_url", image_url ? image_url : "");

        size_t ca_cert_len = 0;
        config_manager_get_ca_cert_der(&ca_cert_len);
        cJSON_AddBoolToObject(root, "ca_cert_set", ca_cert_len > 0);

        const char *fetch_error = utils_get_last_fetch_error();
        if (fetch_error && strlen(fetch_error) > 0) {
            cJSON_AddStringToObject(root, "last_fetch_error", fetch_error);
        }

        const char *access_token = config_manager_get_access_token();
        cJSON_AddStringToObject(root, "access_token", access_token ? access_token : "");
        // The HTTP API password is the one secret this endpoint does NOT
        // return. It is the credential guarding this very endpoint, so
        // serving it here would be circular -- anyone who reaches /api/config
        // once, before authentication is switched on or through any gap,
        // would walk away with the password meant to stop them. Report only
        // whether one is set; nothing needs the value back. (The access token
        // above is different: the server issues and can rotate it, and with
        // authentication off this endpoint exposes far more than that anyway.)
        const char *http_password = config_manager_get_http_password();
        cJSON_AddBoolToObject(root, "http_auth_enabled", http_password && http_password[0] != '\0');

        const char *http_header_key = config_manager_get_http_header_key();
        cJSON_AddStringToObject(root, "http_header_key", http_header_key ? http_header_key : "");

        const char *http_header_value = config_manager_get_http_header_value();
        cJSON_AddStringToObject(root, "http_header_value",
                                http_header_value ? http_header_value : "");

        cJSON_AddBoolToObject(root, "save_downloaded_images",
                              config_manager_get_save_downloaded_images());

        // Home Assistant
        const char *ha_url = config_manager_get_ha_url();
        cJSON_AddStringToObject(root, "ha_url", ha_url ? ha_url : "");
        cJSON_AddBoolToObject(root, "ha_enabled", config_manager_get_ha_enabled());

        // Telegram Bot
        const char *tg_token = config_manager_get_telegram_bot_token();
        cJSON_AddStringToObject(root, "telegram_bot_token", tg_token ? tg_token : "");
        const char *tg_chat_id = config_manager_get_telegram_chat_id();
        cJSON_AddStringToObject(root, "telegram_chat_id", tg_chat_id ? tg_chat_id : "");
        cJSON_AddBoolToObject(root, "telegram_configured", config_manager_telegram_is_configured());
        cJSON_AddBoolToObject(root, "telegram_pairing_enabled",
                              config_manager_get_telegram_pairing_enabled());
        cJSON_AddBoolToObject(root, "telegram_wake_notify_enabled",
                              config_manager_get_telegram_wake_notify_enabled());

        // AI API Keys
        const char *openai_key = config_manager_get_openai_api_key();
        const char *google_key = config_manager_get_google_api_key();
        cJSON_AddStringToObject(root, "openai_api_key", openai_key ? openai_key : "");
        cJSON_AddStringToObject(root, "google_api_key", google_key ? google_key : "");

        // Other
        cJSON_AddBoolToObject(root, "deep_sleep_enabled", config_manager_get_deep_sleep_enabled());
        cJSON_AddBoolToObject(root, "debug_log_enabled", config_manager_get_debug_log_enabled());
        cJSON_AddBoolToObject(root, "ota_check_enabled", config_manager_get_ota_check_enabled());
        cJSON_AddBoolToObject(root, "error_overlay_enabled",
                              config_manager_get_error_overlay_enabled());
        cJSON_AddBoolToObject(root, "wifi_performance_mode_enabled",
                              config_manager_get_wifi_performance_mode_enabled());
        cJSON_AddBoolToObject(root, "wifi_tx_power_cap_enabled",
                              config_manager_get_wifi_tx_power_cap_enabled());
        cJSON_AddBoolToObject(root, "wifi_extended_retry_enabled",
                              config_manager_get_wifi_extended_retry_enabled());
        cJSON_AddBoolToObject(root, "wifi_reprovision_on_fail_enabled",
                              config_manager_get_wifi_reprovision_on_fail_enabled());
        // Read-only here - only ever set during initial setup (offline
        // checkbox, wifi_provisioning.c) or by leaving the on-demand hotspot
        // running is unrelated. Turning it back off needs real credentials,
        // i.e. re-provisioning, not a PATCH.
        cJSON_AddBoolToObject(root, "offline_mode_enabled",
                              config_manager_get_offline_mode_enabled());
        cJSON_AddBoolToObject(root, "ap_hotspot_active", wifi_manager_is_ap_hotspot_active());
        cJSON_AddBoolToObject(root, "https_enabled", config_manager_get_https_enabled());
        cJSON_AddBoolToObject(root, "rotation_pairing_enabled",
                              config_manager_get_rotation_pairing_enabled());
        cJSON_AddBoolToObject(root, "variant_selection_enabled",
                              config_manager_get_variant_selection_enabled());
        cJSON_AddBoolToObject(root, "telegram_rotation_notify_enabled",
                              config_manager_get_telegram_rotation_notify_enabled());
        cJSON_AddBoolToObject(root, "telegram_fallback_rotation_enabled",
                              config_manager_get_telegram_fallback_rotation_enabled());
        cJSON_AddBoolToObject(root, "telegram_fallback_on_error_enabled",
                              config_manager_get_telegram_fallback_on_error_enabled());
        cJSON_AddBoolToObject(root, "telegram_power_save_enabled",
                              config_manager_get_telegram_power_save_enabled());
        cJSON_AddBoolToObject(root, "telegram_power_save_latest_only",
                              config_manager_get_telegram_power_save_latest_only());
        cJSON_AddBoolToObject(root, "telegram_keep_originals_enabled",
                              config_manager_get_telegram_keep_originals_enabled());
        cJSON_AddStringToObject(root, "telegram_image_format",
                                config_manager_get_telegram_image_format());
        cJSON_AddBoolToObject(root, "telegram_dedup_enabled",
                              config_manager_get_telegram_dedup_enabled());
        cJSON_AddBoolToObject(root, "weather_overlay_enabled",
                              config_manager_get_weather_overlay_enabled());
        cJSON_AddStringToObject(root, "weather_location_name",
                                config_manager_get_weather_location_name());
        cJSON_AddStringToObject(root, "weather_lat", config_manager_get_weather_lat());
        cJSON_AddStringToObject(root, "weather_lon", config_manager_get_weather_lon());
        cJSON_AddStringToObject(root, "weather_provider", config_manager_get_weather_provider());
        cJSON_AddBoolToObject(root, "headlines_overlay_enabled",
                              config_manager_get_headlines_overlay_enabled());
        cJSON_AddStringToObject(root, "headlines_rss_url", config_manager_get_headlines_rss_url());
        cJSON_AddNumberToObject(root, "headlines_count", config_manager_get_headlines_count());
        cJSON_AddBoolToObject(root, "overlay_invert_colors",
                              config_manager_get_overlay_invert_colors());
        cJSON_AddBoolToObject(root, "overlay_epdgz_enabled",
                              config_manager_get_overlay_epdgz_enabled());
        cJSON_AddStringToObject(root, "overlay_language", config_manager_get_overlay_language());
        cJSON_AddNumberToObject(root, "headlines_wrap_lines",
                                config_manager_get_headlines_wrap_lines());
        cJSON_AddBoolToObject(root, "caption_invert_colors_enabled",
                              config_manager_get_caption_invert_colors_enabled());
        cJSON_AddBoolToObject(root, "weather_multiline_enabled",
                              config_manager_get_weather_multiline_enabled());
        cJSON_AddStringToObject(root, "weather_icon_set", config_manager_get_weather_icon_set());
        cJSON_AddBoolToObject(root, "weather_icon_colored",
                              config_manager_get_weather_icon_colored());
        cJSON_AddBoolToObject(root, "show_exif_datetime_enabled",
                              config_manager_get_show_exif_datetime_enabled());
        cJSON_AddBoolToObject(root, "low_battery_overlay_enabled",
                              config_manager_get_low_battery_overlay_enabled());
        cJSON_AddNumberToObject(root, "low_battery_overlay_threshold",
                                config_manager_get_low_battery_overlay_threshold());
        cJSON_AddBoolToObject(root, "battery_history_backup_enabled",
                              config_manager_get_battery_history_backup_enabled());
        // Hardware capability, not a user setting - lets the Web UI hide the
        // whole Chimes tab on boards with no onboard speaker.
        cJSON_AddBoolToObject(root, "chime_speaker_available", board_hal_has_speaker());
        const char *chime_mode_str = "off";
        switch (config_manager_get_chime_speaker_mode()) {
        case CHIME_SPEAKER_BATTERY_AND_MAINS:
            chime_mode_str = "battery_and_mains";
            break;
        case CHIME_SPEAKER_MAINS_ONLY:
            chime_mode_str = "mains_only";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(root, "chime_speaker_mode", chime_mode_str);
        cJSON_AddNumberToObject(root, "chime_volume", config_manager_get_chime_volume());
        cJSON_AddBoolToObject(root, "chime_quiet_enabled",
                              config_manager_get_chime_quiet_enabled());
        cJSON_AddStringToObject(root, "chime_quiet_start", config_manager_get_chime_quiet_start());
        cJSON_AddStringToObject(root, "chime_quiet_end", config_manager_get_chime_quiet_end());
        cJSON_AddBoolToObject(root, "chime_event_rotation_enabled",
                              config_manager_get_chime_event_enabled(CHIME_EVENT_ROTATION));
        cJSON_AddBoolToObject(root, "chime_event_telegram_photo_enabled",
                              config_manager_get_chime_event_enabled(CHIME_EVENT_TELEGRAM_PHOTO));
        cJSON_AddBoolToObject(root, "chime_event_low_battery_enabled",
                              config_manager_get_chime_event_enabled(CHIME_EVENT_LOW_BATTERY));
        cJSON_AddBoolToObject(root, "chime_event_wifi_reprovision_enabled",
                              config_manager_get_chime_event_enabled(CHIME_EVENT_WIFI_REPROVISION));
        cJSON_AddBoolToObject(root, "chime_event_agenda_due_enabled",
                              config_manager_get_chime_event_enabled(CHIME_EVENT_AGENDA_DUE));
        cJSON_AddBoolToObject(root, "chime_event_ota_success_enabled",
                              config_manager_get_chime_event_enabled(CHIME_EVENT_OTA_SUCCESS));
        cJSON_AddBoolToObject(root, "chime_event_critical_error_enabled",
                              config_manager_get_chime_event_enabled(CHIME_EVENT_CRITICAL_ERROR));

        // Climate (SHTC3 temperature/humidity). Generic feature - available
        // on any board whose sensor actually answers, not tied to one
        // specific board like the Chimes speaker check above. A live probe
        // (not a compile-time capability flag) since the same board_hal
        // function can fail at runtime even where the driver is wired up
        // (unpowered rail, no sensor populated on a given unit, etc.).
        float climate_probe_temp, climate_probe_hum;
        bool climate_sensor_available =
            (board_hal_get_temperature(&climate_probe_temp) == ESP_OK) &&
            (board_hal_get_humidity(&climate_probe_hum) == ESP_OK);
        cJSON_AddBoolToObject(root, "climate_sensor_available", climate_sensor_available);
        const char *climate_room_str = "living_room";
        switch (config_manager_get_climate_room_type()) {
        case CLIMATE_ROOM_BEDROOM:
            climate_room_str = "bedroom";
            break;
        case CLIMATE_ROOM_BATHROOM:
            climate_room_str = "bathroom";
            break;
        case CLIMATE_ROOM_KITCHEN:
            climate_room_str = "kitchen";
            break;
        case CLIMATE_ROOM_BASEMENT:
            climate_room_str = "basement";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(root, "climate_room_type", climate_room_str);
        cJSON_AddStringToObject(root, "climate_temp_unit",
                                config_manager_get_climate_temp_unit() == CLIMATE_UNIT_FAHRENHEIT
                                    ? "fahrenheit"
                                    : "celsius");
        cJSON_AddBoolToObject(root, "climate_logging_enabled",
                              config_manager_get_climate_logging_enabled());
        cJSON_AddBoolToObject(root, "climate_history_backup_enabled",
                              config_manager_get_climate_history_backup_enabled());
        cJSON_AddBoolToObject(root, "climate_overlay_enabled",
                              config_manager_get_climate_overlay_enabled());
        cJSON_AddBoolToObject(root, "climate_agenda_header_enabled",
                              config_manager_get_climate_agenda_header_enabled());
        // Always Celsius/percentage-point deltas, regardless of
        // climate_temp_unit - the Web UI converts for display in whichever
        // unit is selected (see climate_temp_offset_c's doc comment,
        // config.h).
        cJSON_AddNumberToObject(root, "climate_temp_offset",
                                atof(config_manager_get_climate_temp_offset()));
        cJSON_AddNumberToObject(root, "climate_hum_offset",
                                atof(config_manager_get_climate_hum_offset()));

        // Agenda (ToDo + Calendar). agenda_cal_url/agenda_todo_url are
        // deliberately NEVER added here - either can carry a credential
        // (Google's Calendar "secret address" is the obvious case, but a
        // ToDo feed URL can just as easily embed an auth token as a query
        // param - todo_fetch()/http_fetch_get() don't care what kind of
        // URL they're given), same write-only treatment as wifi_password
        // above, which is also absent from this response. (Originally only
        // agenda_cal_url got this treatment, on the assumption a ToDo feed
        // is typically a public gist - found during a security review that
        // the assumption doesn't hold for every possible ToDo source.)
        cJSON_AddBoolToObject(root, "agenda_todo_enabled",
                              config_manager_get_agenda_todo_enabled());
        cJSON_AddBoolToObject(root, "agenda_cal_enabled", config_manager_get_agenda_cal_enabled());
        cJSON_AddNumberToObject(root, "agenda_cal_days", config_manager_get_agenda_cal_days());
        const char *agenda_cal_layout_str = "list";
        switch (config_manager_get_agenda_cal_layout_mode()) {
        case AGENDA_CAL_LAYOUT_GRID_A:
            agenda_cal_layout_str = "grid_a";
            break;
        case AGENDA_CAL_LAYOUT_GRID_B:
            agenda_cal_layout_str = "grid_b";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(root, "agenda_cal_layout_mode", agenda_cal_layout_str);
        const char *agenda_shift_model_str = "none";
        switch (config_manager_get_agenda_shift_model()) {
        case AGENDA_SHIFT_MODEL_2_2_3:
            agenda_shift_model_str = "2-2-3";
            break;
        case AGENDA_SHIFT_MODEL_WEEK_WEEK:
            agenda_shift_model_str = "week_week";
            break;
        case AGENDA_SHIFT_MODEL_3_4:
            agenda_shift_model_str = "3-4";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(root, "agenda_shift_model", agenda_shift_model_str);
        cJSON_AddStringToObject(root, "agenda_shift_start",
                                config_manager_get_agenda_shift_start());
        cJSON_AddBoolToObject(root, "agenda_cal_weather_enabled",
                              config_manager_get_agenda_cal_weather_enabled());
        cJSON_AddBoolToObject(root, "agenda_cal_weather_right_aligned",
                              config_manager_get_agenda_cal_weather_right_aligned());
        const char *agenda_multiday_str = "repeat";
        switch (config_manager_get_agenda_cal_multiday_mode()) {
        case AGENDA_MULTIDAY_COMPACT:
            agenda_multiday_str = "compact";
            break;
        case AGENDA_MULTIDAY_REPEAT_NUMBERED:
            agenda_multiday_str = "repeat_numbered";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(root, "agenda_cal_multiday_mode", agenda_multiday_str);
        const char *agenda_time_str = "off";
        switch (config_manager_get_agenda_cal_time_display_mode()) {
        case AGENDA_TIME_DISPLAY_DURATION:
            agenda_time_str = "duration";
            break;
        case AGENDA_TIME_DISPLAY_RANGE:
            agenda_time_str = "range";
            break;
        default:
            break;
        }
        cJSON_AddStringToObject(root, "agenda_cal_time_display_mode", agenda_time_str);
        cJSON_AddStringToObject(root, "agenda_cal_name", config_manager_get_agenda_cal_name());
        cJSON_AddStringToObject(root, "agenda_cal_name2", config_manager_get_agenda_cal_name2());
        // Non-secret "is a source actually saved?" flags - the URL fields
        // themselves are write-only (see the comment above), so without
        // these the Web UI has no way to tell a freshly-saved, working
        // calendar apart from one that was enabled but never actually given
        // a URL/file, both before and after a page reload. A/B: was a URL
        // ever saved. C/D/E: is there a raw .ics file on disk right now
        // (from a URL fetch or a direct upload) - matches exactly what
        // load_extra_ics_source() in agenda_manager.c needs to find
        // anything at all.
        cJSON_AddBoolToObject(root, "agenda_cal_url_configured",
                              config_manager_get_agenda_cal_url()[0] != '\0');
        cJSON_AddBoolToObject(root, "agenda_cal_url2_configured",
                              config_manager_get_agenda_cal_url2()[0] != '\0');
        struct stat cal_c_st, cal_d_st, cal_e_st;
        cJSON_AddBoolToObject(root, "agenda_cal_c_configured",
                              stat(AGENDA_CAL_CACHE_PATH_C, &cal_c_st) == 0);
        cJSON_AddBoolToObject(root, "agenda_cal_d_configured",
                              stat(AGENDA_CAL_CACHE_PATH_D, &cal_d_st) == 0);
        cJSON_AddBoolToObject(root, "agenda_cal_e_configured",
                              stat(AGENDA_CAL_CACHE_PATH_E, &cal_e_st) == 0);
        // Three extra ICS sources (e.g. holidays/school-holidays) - same
        // write-only URL treatment as agenda_cal_url/_url2 above, but their
        // enabled flag/name/color are plain, non-secret settings.
        cJSON_AddBoolToObject(root, "agenda_cal_c_enabled",
                              config_manager_get_agenda_cal_c_enabled());
        cJSON_AddStringToObject(root, "agenda_cal_c_name", config_manager_get_agenda_cal_c_name());
        cJSON_AddBoolToObject(root, "agenda_cal_d_enabled",
                              config_manager_get_agenda_cal_d_enabled());
        cJSON_AddStringToObject(root, "agenda_cal_d_name", config_manager_get_agenda_cal_d_name());
        cJSON_AddBoolToObject(root, "agenda_cal_e_enabled",
                              config_manager_get_agenda_cal_e_enabled());
        cJSON_AddStringToObject(root, "agenda_cal_e_name", config_manager_get_agenda_cal_e_name());
        cJSON *agenda_cron_arr = cJSON_CreateArray();
        int agenda_cron_count = config_manager_get_agenda_cron_rule_count();
        for (int i = 0; i < agenda_cron_count; i++) {
            const char *rule = config_manager_get_agenda_cron_rule(i);
            if (rule) {
                cJSON_AddItemToArray(agenda_cron_arr, cJSON_CreateString(rule));
            }
        }
        cJSON_AddItemToObject(root, "agenda_cron", agenda_cron_arr);

        // Alarm Clock - always reported (not just on a build compiled with
        // CONFIG_ALARM_CLOCK_ENABLED): config_manager_get_alarm_*() and
        // alarm_manager_is_compiled_in() are harmless no-ops on every other
        // build, so the Web UI always sees a well-formed but empty/disabled
        // shape and can decide for itself (via alarm_clock_available)
        // whether to show the settings tab at all.
        cJSON_AddBoolToObject(root, "alarm_clock_available", alarm_manager_is_compiled_in());
        cJSON *alarm_cron_arr = cJSON_CreateArray();
        int alarm_cron_count = config_manager_get_alarm_cron_rule_count();
        for (int i = 0; i < alarm_cron_count; i++) {
            const char *rule = config_manager_get_alarm_cron_rule(i);
            if (rule) {
                cJSON_AddItemToArray(alarm_cron_arr, cJSON_CreateString(rule));
            }
        }
        cJSON_AddItemToObject(root, "alarm_cron", alarm_cron_arr);
        cJSON_AddNumberToObject(root, "alarm_ring_duration_sec",
                                config_manager_get_alarm_ring_duration_sec());

        cJSON_AddBoolToObject(root, "agenda_stack_layout",
                              config_manager_get_agenda_stack_layout());
        cJSON_AddNumberToObject(root, "agenda_color_profile_active",
                                config_manager_get_agenda_color_profile_active());
        cJSON_AddStringToObject(root, "agenda_pri_a_color",
                                config_manager_get_agenda_pri_a_color());
        cJSON_AddStringToObject(root, "agenda_pri_b_color",
                                config_manager_get_agenda_pri_b_color());
        cJSON_AddStringToObject(root, "agenda_pri_c_color",
                                config_manager_get_agenda_pri_c_color());
        cJSON_AddStringToObject(root, "agenda_pri_d_color",
                                config_manager_get_agenda_pri_d_color());
        cJSON_AddStringToObject(root, "agenda_due_overdue_color",
                                config_manager_get_agenda_due_overdue_color());
        cJSON_AddStringToObject(root, "agenda_due_today_color",
                                config_manager_get_agenda_due_today_color());
        cJSON_AddStringToObject(root, "agenda_due_later_color",
                                config_manager_get_agenda_due_later_color());
        cJSON_AddStringToObject(root, "agenda_project_color",
                                config_manager_get_agenda_project_color());
        cJSON_AddStringToObject(root, "agenda_context_color",
                                config_manager_get_agenda_context_color());

        char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(root);

        return ESP_OK;
    } else if (req->method == HTTP_POST || req->method == HTTP_PATCH) {
        size_t buf_size = req->content_len + 1;
        // 32768: this endpoint's own settings surface has grown well past
        // what the original 4096-byte cap here allowed for - confirmed live
        // (2026-09-20) that a full Settings-page "Export Config" JSON (with
        // "Include credentials and URLs" on: 3 Agenda Calendar URLs + a ToDo
        // URL + Telegram token/chat ID pushing it past 4.7KB) got REJECTED
        // outright by this check on re-import, silently dropping every
        // field in one shot - the Vue side's Promise.all() doesn't check
        // response.ok, so the UI reported "imported successfully" anyway
        // (see webapp's performImport() fix, same incident). PSRAM-backed
        // since this buffer can now be meaningfully large; freed well before
        // the eventual e-paper render pipeline would need that RAM back.
        if (buf_size > 32768) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
            return ESP_FAIL;
        }
        char *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (!buf) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return ESP_FAIL;
        }

        int received = 0;
        while (received < req->content_len) {
            int ret = httpd_req_recv(req, buf + received, req->content_len - received);
            if (ret <= 0) {
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
                return ESP_FAIL;
            }
            received += ret;
        }
        buf[received] = '\0';

        // Never log the body: it can carry WiFi credentials, API keys and the
        // device's own HTTP password, and the debug log is persisted and
        // served back over /api/debug/log.
        ESP_LOGD(TAG, "Config %s request (%d bytes)", req->method == HTTP_PATCH ? "PATCH" : "POST",
                 received);

        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (!root) {
            ESP_LOGW(TAG, "Config request rejected: invalid JSON");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        esp_err_t apply_result = apply_config_from_json(root, false);
        cJSON_Delete(root);

        if (apply_result != ESP_OK) {
            cJSON *error_response = cJSON_CreateObject();
            cJSON_AddStringToObject(error_response, "status", "error");

            const char *config_err = utils_consume_config_error();
            const char *cert_err = utils_consume_cert_pin_error();
            ESP_LOGW(TAG, "Config apply failed (config_err='%s', cert_err='%s')", config_err,
                     cert_err);
            if (config_err && config_err[0] != '\0') {
                cJSON_AddStringToObject(error_response, "message", config_err);
            } else if (cert_err && cert_err[0] != '\0') {
                char msg[384];
                snprintf(msg, sizeof(msg), "Failed to pin TLS certificate for image URL: %s",
                         cert_err);
                cJSON_AddStringToObject(error_response, "message", msg);
            } else {
                cJSON_AddStringToObject(
                    error_response, "message",
                    "Failed to connect to WiFi network. Please check SSID and password.");
            }

            char *json_str = cJSON_Print(error_response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, json_str);

            free(json_str);
            cJSON_Delete(error_response);
            return ESP_FAIL;
        }

        // Update config timestamp for remote sync
        config_manager_touch_config();

        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "success");

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);

        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

// Deliberately NOT part of GET /api/config's response (see the write-only
// comment on agenda_todo_url/agenda_cal_url etc. there - either can carry a
// credential embedded as a query param) - this exists only so the Web UI's
// "Export Config" opt-in checkbox can include a fully self-contained
// backup on request, without these URLs being readable on every normal
// Settings-page load. Same fields, same plain-text-JSON exposure as the
// existing credential fields GET /api/config already returns unconditionally
// - reachable by anyone who can reach this device's HTTP server either way.
static esp_err_t config_urls_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_sendstr(req, "Failed to create JSON response");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "agenda_todo_url", config_manager_get_agenda_todo_url());
    cJSON_AddStringToObject(root, "agenda_cal_url", config_manager_get_agenda_cal_url());
    cJSON_AddStringToObject(root, "agenda_cal_url2", config_manager_get_agenda_cal_url2());
    cJSON_AddStringToObject(root, "agenda_cal_c_url", config_manager_get_agenda_cal_c_url());
    cJSON_AddStringToObject(root, "agenda_cal_d_url", config_manager_get_agenda_cal_d_url());
    cJSON_AddStringToObject(root, "agenda_cal_e_url", config_manager_get_agenda_cal_e_url());

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t albums_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System not ready");
        return ESP_OK;
    }
    if (!storage_has_persistent_storage()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage not found");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        char **albums = NULL;
        int count = 0;
        if (album_manager_list_albums(&albums, &count) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to list albums");
            return ESP_FAIL;
        }

        cJSON *response = cJSON_CreateArray();
        for (int i = 0; i < count; i++) {
            cJSON *album_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(album_obj, "name", albums[i]);
            cJSON_AddBoolToObject(album_obj, "enabled", album_manager_is_album_enabled(albums[i]));
            cJSON_AddItemToArray(response, album_obj);
        }
        album_manager_free_album_list(albums, count);

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    } else if (req->method == HTTP_POST) {
        char buf[256];
        int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request");
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (!root) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        cJSON *name_json = cJSON_GetObjectItem(root, "name");
        if (!name_json || !cJSON_IsString(name_json)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name");
            return ESP_FAIL;
        }

        const char *album_name = name_json->valuestring;
        esp_err_t err = album_manager_create_album(album_name);
        cJSON_Delete(root);

        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create album");
            return ESP_FAIL;
        }

        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "success");
        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

static esp_err_t album_delete_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System not ready");
        return ESP_OK;
    }
    if (!storage_has_persistent_storage()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage not found");
        return ESP_FAIL;
    }

    // Use query parameter since ESP-IDF httpd doesn't support wildcard URIs
    char query[256];
    char album_name[128];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "name", album_name, sizeof(album_name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name parameter");
        return ESP_FAIL;
    }

    // URL decode the album name to handle special characters like '+'
    char decoded_album_name[128];
    url_decode(decoded_album_name, album_name, sizeof(decoded_album_name));

    esp_err_t err = album_manager_delete_album(decoded_album_name);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete album");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t album_enabled_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System not ready");
        return ESP_OK;
    }
    if (!storage_has_persistent_storage()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage not found");
        return ESP_FAIL;
    }

    // Get album name from query parameter
    char query[256];
    char album_name[128];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(query, "name", album_name, sizeof(album_name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album name parameter");
        return ESP_FAIL;
    }

    // URL decode the album name to handle special characters like '+'
    char decoded_album_name[128];
    url_decode(decoded_album_name, album_name, sizeof(decoded_album_name));

    // Get enabled status from JSON body
    char buf[256];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *enabled_json = cJSON_GetObjectItem(root, "enabled");

    if (!enabled_json || !cJSON_IsBool(enabled_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled field");
        return ESP_FAIL;
    }

    bool enabled = cJSON_IsTrue(enabled_json);

    esp_err_t err = album_manager_set_album_enabled(decoded_album_name, enabled);
    cJSON_Delete(root);

    // Enabling an album whose folder doesn't exist on THIS device yet (album
    // folders are created by uploading a photo into them, not by config) is
    // a client-side "not found" condition, not a server fault - surfacing it
    // as a generic 500 (as this used to) reads as a firmware bug to anyone
    // importing a config exported from a different device with a different
    // photo library. Disabling a nonexistent album is unaffected (see
    // album_manager_set_album_enabled()'s own comment) and still succeeds.
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                            "Album does not exist on this device - upload at least one photo "
                            "to it first");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_TIMEOUT) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "Album list is busy, try again");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to update album");
        return ESP_FAIL;
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t album_images_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System not ready");
        return ESP_OK;
    }
    if (!storage_has_persistent_storage()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Storage not found");
        return ESP_FAIL;
    }

    char query[256];
    char album_name[128] = "";
    bool include_thumbnails = true;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "album", album_name, sizeof(album_name));
        char thumbnails_param[8] = "";
        if (httpd_query_key_value(query, "thumbnails", thumbnails_param,
                                  sizeof(thumbnails_param)) == ESP_OK) {
            include_thumbnails = (strcmp(thumbnails_param, "0") != 0);
        }
    }

    if (strlen(album_name) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing album parameter");
        return ESP_FAIL;
    }

    // URL decode the album name to handle special characters like '+'
    char decoded_album_name[128];
    url_decode(decoded_album_name, album_name, sizeof(decoded_album_name));

    char album_path[256];
    if (album_manager_get_album_path(decoded_album_name, album_path, sizeof(album_path)) !=
        ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid album");
        return ESP_FAIL;
    }

    DIR *dir = opendir(album_path);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open album directory");
        return ESP_FAIL;
    }

    // Collect the base names of every thumbnail (".jpg", never used by a main
    // image - see the extension check below) present in this album with a
    // single readdir() pass, so the loop below can check existence via an
    // in-memory string compare instead of a stat() syscall per image. On a
    // large album (hundreds of images) that used to mean hundreds of
    // sequential storage lookups inside one handler call - since
    // esp_http_server processes requests on a single task, that blocked the
    // entire Web UI (not just this request) for as long as the scan ran.
    char(*thumb_bases)[256] = NULL;
    size_t thumb_count = 0;
    size_t thumb_capacity = 0;
    if (include_thumbnails) {
        struct dirent *tentry;
        while ((tentry = readdir(dir)) != NULL) {
            if (tentry->d_type != DT_REG) {
                continue;
            }
            const char *tent_ext = strrchr(tentry->d_name, '.');
            if (!tent_ext || strcasecmp(tent_ext, ".jpg") != 0) {
                continue;
            }
            if (thumb_count == thumb_capacity) {
                size_t new_capacity = thumb_capacity == 0 ? 32 : thumb_capacity * 2;
                char(*grown)[256] = heap_caps_realloc(thumb_bases, new_capacity * sizeof(*grown),
                                                      MALLOC_CAP_SPIRAM);
                if (!grown) {
                    break;  // Keep what we have - a missed thumbnail just falls back to the
                            // placeholder icon.
                }
                thumb_bases = grown;
                thumb_capacity = new_capacity;
            }
            int tbase_len = (int) (tent_ext - tentry->d_name);
            snprintf(thumb_bases[thumb_count], sizeof(thumb_bases[0]), "%.*s", tbase_len,
                     tentry->d_name);
            thumb_count++;
        }
        rewinddir(dir);
    }

    cJSON *response = cJSON_CreateArray();
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                continue;
            }
            const char *ext = strrchr(entry->d_name, '.');
            if (ext &&
                (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
                 strcasecmp(ext, ".epdgz") == 0) &&
                display_manager_is_photo_anchor(album_path, entry->d_name)) {
                cJSON *image_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(image_obj, "filename", entry->d_name);
                cJSON_AddStringToObject(image_obj, "album", decoded_album_name);

                // Check if a corresponding JPG-named thumbnail exists (Web UI
                // uploads generate a real one client-side; Telegram downloads
                // get one generated server-side - see
                // generate_original_thumbnail() in telegram_bot.c - as
                // PNG-encoded bytes under a ".jpg" name, since the firmware
                // has no JPEG encoder; browsers sniff content, not
                // extension, so this displays fine either way). Always a
                // different filename than the main image itself, since a
                // ".jpg" thumbnail can never collide with a listed
                // .bmp/.png/.epdgz main image. Skipped entirely when the
                // client doesn't want thumbnails (Web UI "Show thumbnails"
                // off). Checked against the thumb_bases[] set collected
                // above instead of stat()-ing the candidate path directly -
                // see the comment above that pass for why.
                if (include_thumbnails) {
                    int base_len = (int) (ext - entry->d_name);
                    bool has_thumb = false;
                    for (size_t i = 0; i < thumb_count; i++) {
                        if ((int) strlen(thumb_bases[i]) == base_len &&
                            strncmp(thumb_bases[i], entry->d_name, base_len) == 0) {
                            has_thumb = true;
                            break;
                        }
                    }
                    if (has_thumb) {
                        char thumbnail_name[256];
                        snprintf(thumbnail_name, sizeof(thumbnail_name), "%.*s.jpg", base_len,
                                 entry->d_name);
                        cJSON_AddStringToObject(image_obj, "thumbnail", thumbnail_name);
                    }
                }

                cJSON_AddItemToArray(response, image_obj);
            }
        }
    }
    closedir(dir);
    free(thumb_bases);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t system_info_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON *response = cJSON_CreateObject();

    cJSON_AddStringToObject(response, "device_name", config_manager_get_device_name());
    cJSON_AddStringToObject(response, "device_id", get_device_id());
    cJSON_AddNumberToObject(response, "width", BOARD_HAL_DISPLAY_WIDTH);
    cJSON_AddNumberToObject(response, "height", BOARD_HAL_DISPLAY_HEIGHT);
    cJSON_AddStringToObject(response, "board_name", BOARD_HAL_NAME);
    cJSON_AddStringToObject(response, "display_type", BOARD_HAL_DISPLAY_TYPE);
    cJSON_AddStringToObject(response, "wakeup_key_name", BOARD_HAL_WAKEUP_KEY_NAME);

    storage_type_t storage_type = storage_get_type();
    bool sdcard_inserted = (storage_type == STORAGE_TYPE_SDCARD);
    bool has_flash_storage = (storage_type == STORAGE_TYPE_LITTLEFS);
    uint64_t storage_total = 0;
    uint64_t storage_used = 0;

    if (sdcard_inserted) {
        uint64_t t = 0, f = 0;
        if (esp_vfs_fat_info(FS_MOUNT_POINT, &t, &f) == ESP_OK) {
            storage_total = t;
            storage_used = t - f;
        }
    } else if (has_flash_storage) {
        size_t t = 0, u = 0;
        if (esp_littlefs_info(LITTLEFS_PARTITION_LABEL, &t, &u) == ESP_OK) {
            storage_total = t;
            storage_used = u;
        }
    }

#ifdef CONFIG_HAS_SDCARD
    cJSON_AddBoolToObject(response, "has_sdcard", true);
#else
    cJSON_AddBoolToObject(response, "has_sdcard", false);
#endif
    cJSON_AddBoolToObject(response, "sdcard_inserted", sdcard_inserted);
    cJSON_AddBoolToObject(response, "has_flash_storage", has_flash_storage);

    // Pass along new storage properties
    cJSON_AddNumberToObject(response, "storage_total", storage_total);
    cJSON_AddNumberToObject(response, "storage_used", storage_used);

    cJSON_AddStringToObject(response, "version", app_desc->version);
    cJSON_AddStringToObject(response, "project_name", app_desc->project_name);
    cJSON_AddStringToObject(response, "compile_time", app_desc->time);
    cJSON_AddStringToObject(response, "compile_date", app_desc->date);
    cJSON_AddStringToObject(response, "idf_version", app_desc->idf_ver);

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ota_status_t status;
    ota_get_status(&status);

    cJSON *response = cJSON_CreateObject();

    // Add state as string
    const char *state_str = "idle";
    switch (status.state) {
    case OTA_STATE_IDLE:
        state_str = "idle";
        break;
    case OTA_STATE_CHECKING:
        state_str = "checking";
        break;
    case OTA_STATE_UPDATE_AVAILABLE:
        state_str = "update_available";
        break;
    case OTA_STATE_DOWNLOADING:
        state_str = "downloading";
        break;
    case OTA_STATE_INSTALLING:
        state_str = "installing";
        break;
    case OTA_STATE_SUCCESS:
        state_str = "success";
        break;
    case OTA_STATE_ERROR:
        state_str = "error";
        break;
    }

    cJSON_AddStringToObject(response, "state", state_str);
    cJSON_AddStringToObject(response, "current_version", status.current_version);
    cJSON_AddStringToObject(response, "latest_version", status.latest_version);
    cJSON_AddNumberToObject(response, "progress_percent", status.progress_percent);

    if (status.error_message[0] != '\0') {
        cJSON_AddStringToObject(response, "error_message", status.error_message);
    }

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t ota_check_handler(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        bool update_available = false;
        esp_err_t err = ota_check_for_update(&update_available, 30);

        cJSON *response = cJSON_CreateObject();
        if (err == ESP_OK) {
            cJSON_AddBoolToObject(response, "update_available", update_available);
            cJSON_AddStringToObject(response, "status", "success");
        } else {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message", "Failed to check for updates");
        }

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        esp_err_t err = ota_start_update();

        cJSON *response = cJSON_CreateObject();
        if (err == ESP_OK) {
            cJSON_AddStringToObject(response, "status", "success");
            cJSON_AddStringToObject(response, "message", "OTA update started");
        } else if (err == ESP_ERR_INVALID_STATE) {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message",
                                    "No update available or update already in progress");
        } else {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message", "Failed to start OTA update");
        }

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

static esp_err_t keep_alive_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    // Reset sleep timer to keep device awake while webapp is actively being used
    power_manager_reset_sleep_timer();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");

    return ESP_OK;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));  // Wait 1 second for response to be sent
    ESP_LOGI(TAG, "Restarting device...");
    esp_restart();
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Factory reset requested");

    // Erase all NVS data first
    ESP_LOGI(TAG, "Erasing NVS flash...");
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to erase NVS\"}");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "NVS erased successfully");

    // The Agenda ETag cache files live on the SD card/internal flash, not in
    // NVS - erasing NVS alone would leave them orphaned (their matching NVS
    // ETag validators are gone, so they'd never be read again, just sitting
    // there unused). Best-effort: a factory reset should leave storage as
    // clean as the config it just wiped. A missing file (e.g. Agenda was
    // never enabled) is expected, not an error - logged at INFO either way
    // so a factory reset's actual cleanup effect is visible in the log
    // rather than silently assumed.
    const char *agenda_cache_paths[] = {
        AGENDA_TODO_CACHE_PATH,       AGENDA_CAL_CACHE_PATH,        AGENDA_CAL_CACHE_PATH2,
        AGENDA_CAL_CACHE_PATH_C,      AGENDA_CAL_CACHE_PATH_D,      AGENDA_CAL_CACHE_PATH_E,
        AGENDA_CAL_CACHE_PATH_C_FLAT, AGENDA_CAL_CACHE_PATH_D_FLAT, AGENDA_CAL_CACHE_PATH_E_FLAT};
    for (size_t i = 0; i < sizeof(agenda_cache_paths) / sizeof(agenda_cache_paths[0]); i++) {
        if (unlink(agenda_cache_paths[i]) == 0) {
            ESP_LOGI(TAG, "Removed orphaned Agenda cache file: %s", agenda_cache_paths[i]);
        } else {
            ESP_LOGI(TAG, "No Agenda cache file to remove at: %s", agenda_cache_paths[i]);
        }
    }

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
                       "{\"status\":\"success\",\"message\":\"Factory reset initiated. Device "
                       "will restart.\"}");

    // Schedule restart in a separate task to allow HTTP response to be sent
    xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);

    return ESP_OK;
}

static esp_err_t display_calibration_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Displaying calibration pattern on e-paper");

    // Generate and display calibration pattern dynamically
    esp_err_t ret = display_manager_show_calibration();

    if (ret == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"status\":\"success\",\"message\":\"Calibration pattern displayed\"}");
        return ESP_OK;
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(
            req, "{\"status\":\"error\",\"message\":\"Failed to display calibration pattern\"}");
        return ESP_FAIL;
    }
}

static esp_err_t error_overlay_test_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Testing error overlay display");

    esp_err_t ret = utils_test_error_overlay();

    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Error overlay displayed\"}");
        return ESP_OK;
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(
            req, "{\"status\":\"error\",\"message\":\"Failed to display error overlay\"}");
        return ESP_FAIL;
    }
}

// POST /api/chimes/test - plays a beep pattern directly on the onboard
// speaker (board_hal_has_speaker()), bypassing every Chimes policy gate
// (master mode, quiet hours, mains-only) on purpose: the whole point of a
// test button is to hear it regardless of current settings. Optional JSON
// body {"pattern": "success"|"warning"|"error"}, defaults to "success".
static esp_err_t chime_test_handler(httpd_req_t *req)
{
    if (!board_hal_has_speaker()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"This board has no speaker\"}");
        return ESP_FAIL;
    }

    board_hal_chime_kind_t kind = BOARD_HAL_CHIME_SUCCESS;
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            cJSON *pattern = cJSON_GetObjectItem(root, "pattern");
            if (pattern && cJSON_IsString(pattern)) {
                const char *p = cJSON_GetStringValue(pattern);
                if (strcmp(p, "warning") == 0) {
                    kind = BOARD_HAL_CHIME_WARNING;
                } else if (strcmp(p, "error") == 0) {
                    kind = BOARD_HAL_CHIME_ERROR;
                }
            }
            cJSON_Delete(root);
        }
    }

    ESP_LOGI(TAG, "Testing speaker chime (kind=%d)", (int) kind);
    // Uses the configured volume, same as a real chime, so the test button
    // shows exactly what the user will actually hear.
    esp_err_t ret = board_hal_play_beep_pattern(kind, (uint8_t) config_manager_get_chime_volume());

    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Chime played\"}");
        return ESP_OK;
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to play chime\"}");
        return ESP_FAIL;
    }
}

// Maximum accepted size for a directly-uploaded extra ICS file - generous
// vs. a realistic holidays/school-holidays/special-days feed (calendar_ics.c
// itself caps a normal fetch at 2MB for a full personal calendar's years of
// history; a hand-curated or single-purpose feed like these is expected to
// be far smaller), but still bounded rather than accepting an arbitrarily
// large body into a heap allocation.
#define AGENDA_EXTRA_ICS_UPLOAD_MAX_BYTES (512 * 1024)

// POST /api/agenda/extra-ics?slot=c|d|e - lets the user upload a .ics file
// directly instead of providing a URL, for one of the three extra Calendar
// sources that never auto-refresh (see NVS_AGENDA_CAL_C_URL_KEY etc. in
// config.h). The raw request body is the .ics content itself (not
// multipart - these are plain text files, unlike the photo uploads
// elsewhere in this file); it's written straight to that slot's cache file,
// with no network fetch involved at all. A minimal sanity check
// ("BEGIN:VCALENDAR" prefix) guards against silently caching something that
// clearly isn't an ICS file, matching this project's fail-soft-but-not-
// blind style elsewhere.
static esp_err_t agenda_extra_ics_upload_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    char query[32];
    char slot[4] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "slot", slot, sizeof(slot)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?slot=c|d|e");
        return ESP_FAIL;
    }
    const char *cache_path;
    const char *flat_cache_path;
    if (strcmp(slot, "c") == 0) {
        cache_path = AGENDA_CAL_CACHE_PATH_C;
        flat_cache_path = AGENDA_CAL_CACHE_PATH_C_FLAT;
    } else if (strcmp(slot, "d") == 0) {
        cache_path = AGENDA_CAL_CACHE_PATH_D;
        flat_cache_path = AGENDA_CAL_CACHE_PATH_D_FLAT;
    } else if (strcmp(slot, "e") == 0) {
        cache_path = AGENDA_CAL_CACHE_PATH_E;
        flat_cache_path = AGENDA_CAL_CACHE_PATH_E_FLAT;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot must be c, d, or e");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > AGENDA_EXTRA_ICS_UPLOAD_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File missing or too large");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    char *buf = heap_caps_malloc((size_t) req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            heap_caps_free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';

    if (strncmp(buf, "BEGIN:VCALENDAR", 15) != 0) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Not an ICS file (missing BEGIN:VCALENDAR)");
        return ESP_FAIL;
    }

    FILE *fp = fopen(cache_path, "wb");
    if (!fp) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save file");
        return ESP_FAIL;
    }
    fwrite(buf, 1, (size_t) received, fp);
    fclose(fp);
    // Invalidate the expanded-cache tier too - otherwise a stale-but-not-
    // yet-exhausted expansion from before this upload would keep being
    // served for up to AGENDA_EXTRA_ICS_EXPAND_DAYS, silently ignoring the
    // file just uploaded (see load_extra_ics_source() in agenda_manager.c).
    unlink(flat_cache_path);
    heap_caps_free(buf);

    ESP_LOGI(TAG, "Extra ICS source '%s' updated via upload (%d bytes)", slot, received);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    return ESP_OK;
}

// GET/POST/DELETE /api/agenda/color-profile?slot=1|2|3 - manages the up-to-
// AGENDA_COLOR_PROFILE_SLOTS stored Calendar-view color profiles imported
// from profile-editor.html's JSON export (see agenda_color_profile.h).
// GET (no ?slot=) lists all slots' names + which one is active; POST
// imports/replaces one slot's profile (raw JSON body, same non-multipart
// convention as agenda_extra_ics_upload_handler() above); DELETE removes
// one slot, clearing the active pointer first if it pointed there.
// Selecting which slot is *active* is a plain scalar setting instead
// (agenda_color_profile_active via PATCH /api/config), not part of this
// endpoint.
static esp_err_t agenda_color_profile_handler(httpd_req_t *req)
{
    if (!system_ready) {
        httpd_resp_set_status(req, HTTPD_503);
        httpd_resp_sendstr(req, "System is still initializing");
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        cJSON *root = cJSON_CreateObject();
        cJSON *slots = cJSON_CreateArray();
        for (int slot = 1; slot <= AGENDA_COLOR_PROFILE_SLOTS; slot++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "slot", slot);
            char name[AGENDA_CAL_CDE_NAME_MAX_LEN * 2];
            if (agenda_color_profile_slot_name(slot, name, sizeof(name))) {
                cJSON_AddStringToObject(entry, "name", name);
            } else {
                cJSON_AddNullToObject(entry, "name");
            }
            cJSON_AddItemToArray(slots, entry);
        }
        cJSON_AddItemToObject(root, "slots", slots);
        cJSON_AddNumberToObject(root, "active", config_manager_get_agenda_color_profile_active());
        char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }

    char query[32];
    char slot_str[4] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "slot", slot_str, sizeof(slot_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?slot=1|2|3");
        return ESP_FAIL;
    }
    int slot = atoi(slot_str);
    if (slot < 1 || slot > AGENDA_COLOR_PROFILE_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot must be 1..3");
        return ESP_FAIL;
    }
    char path[64];
    agenda_color_profile_path(slot, path, sizeof(path));

    if (req->method == HTTP_DELETE) {
        unlink(path);
        if (config_manager_get_agenda_color_profile_active() == slot) {
            config_manager_set_agenda_color_profile_active(0);
        }
        ESP_LOGI(TAG, "Color profile slot %d removed", slot);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
        return ESP_OK;
    }

    // POST: raw JSON body, same non-multipart convention as
    // agenda_extra_ics_upload_handler() above.
    if (req->content_len <= 0 || req->content_len > AGENDA_COLOR_PROFILE_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Profile missing or too large");
        return ESP_FAIL;
    }

    power_manager_reset_sleep_timer();

    char *buf = heap_caps_malloc((size_t) req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            heap_caps_free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';

    char err[96];
    if (!agenda_color_profile_validate(buf, NULL, 0, err, sizeof(err))) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_FAIL;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save profile");
        return ESP_FAIL;
    }
    fwrite(buf, 1, (size_t) received, fp);
    fclose(fp);
    heap_caps_free(buf);

    ESP_LOGI(TAG, "Color profile slot %d updated via import (%d bytes)", slot, received);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    return ESP_OK;
}

static esp_err_t processing_settings_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        processing_settings_t settings;
        if (processing_settings_load(&settings) != ESP_OK) {
            processing_settings_get_defaults(&settings);
        }

        char *json_str = processing_settings_to_json(&settings);
        if (!json_str) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        return ESP_OK;

    } else if (req->method == HTTP_POST) {
        char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        int ret = httpd_req_recv(req, buf, req->content_len);
        if (ret <= 0) {
            heap_caps_free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *json = cJSON_Parse(buf);
        heap_caps_free(buf);

        if (!json) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        processing_settings_t settings;
        processing_settings_get_defaults(&settings);
        processing_settings_from_json(json, &settings);
        cJSON_Delete(json);

        esp_err_t err = processing_settings_save(&settings);
        if (err != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        // Advance the shared config revision: the remote sync only pulls
        // device state whose timestamp moved forward, so without this a
        // later server-side edit would push the server's stale processing
        // settings back over this change
        config_manager_touch_config();

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true}");
        return ESP_OK;

    } else if (req->method == HTTP_DELETE) {
        // Reset to firmware defaults
        processing_settings_t settings;
        processing_settings_get_defaults(&settings);

        // Save defaults to NVS
        esp_err_t err = processing_settings_save(&settings);
        if (err != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        config_manager_touch_config();

        // Return the full default settings via the shared serializer so the
        // response can never drift from the persisted fields (the previous
        // hand-built list had already fallen behind)
        char *json_str = processing_settings_to_json(&settings);
        if (!json_str) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

static esp_err_t time_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Return current device time
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "time", time_str);
        cJSON_AddNumberToObject(response, "timestamp", (double) now);
        cJSON_AddStringToObject(response, "timezone", config_manager_get_timezone());

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

static esp_err_t time_sync_handler(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        ESP_LOGI(TAG, "Manual NTP sync requested");

        // Force SNTP sync
        esp_err_t err = periodic_tasks_force_run(SNTP_TASK_NAME);
        if (err != ESP_OK) {
            cJSON *response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "message", "Failed to trigger NTP sync");

            char *json_str = cJSON_Print(response);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, json_str);

            free(json_str);
            cJSON_Delete(response);
            return ESP_OK;
        }

        // Run the sync immediately
        periodic_tasks_check_and_run();

        // Get the new time
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

        cJSON *response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "time", time_str);
        cJSON_AddNumberToObject(response, "timestamp", (double) now);
        cJSON_AddStringToObject(response, "timezone", config_manager_get_timezone());

        char *json_str = cJSON_Print(response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);

        free(json_str);
        cJSON_Delete(response);
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

static esp_err_t color_palette_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        color_palette_t palette;
        if (color_palette_load(&palette) != ESP_OK) {
            color_palette_get_defaults(&palette);
        }

        char *json_str = color_palette_to_json(&palette);
        if (!json_str) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        return ESP_OK;

    } else if (req->method == HTTP_POST) {
        char *buf = malloc(req->content_len + 1);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        int ret = httpd_req_recv(req, buf, req->content_len);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        buf[ret] = '\0';

        cJSON *json = cJSON_Parse(buf);
        free(buf);

        if (!json) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
            return ESP_FAIL;
        }

        color_palette_t palette;
        color_palette_get_defaults(&palette);
        color_palette_from_json(json, &palette);
        cJSON_Delete(json);

        esp_err_t err = color_palette_save(&palette);
        if (err != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        // Same revision rule as the processing settings above
        config_manager_touch_config();

        // Reload palette in image processor so subsequent uploads use the new calibration
        image_processor_reload_palette();

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true}");
        return ESP_OK;
    } else if (req->method == HTTP_DELETE) {
        // Reset palette to defaults
        color_palette_t palette;
        color_palette_get_defaults(&palette);

        esp_err_t err = color_palette_save(&palette);
        if (err != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        config_manager_touch_config();

        // Reload palette in image processor
        image_processor_reload_palette();

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true}");
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    return ESP_FAIL;
}

// Registers every route this device serves onto whichever handle is passed
// in - the plain-HTTP `server` (always) and, if HTTPS is enabled, a second
// TLS-wrapped instance too (see http_server_init() below). Extracted so both
// instances share one definition instead of two copies drifting apart -
// httpd_ssl_start() (esp_https_server) is a thin wrapper that still hands
// back an ordinary httpd_handle_t, so register_uri() works unchanged on
// either kind of handle. Every route goes through register_uri() so the
// optional HTTP-auth gate (see above) cannot be forgotten when a new
// endpoint is added.
static void register_all_handlers(httpd_handle_t handle)
{
    register_uri(handle, "/", HTTP_GET, index_handler);
    register_uri(handle, "/assets/index.css", HTTP_GET, index_css_handler);
    register_uri(handle, "/assets/index.js", HTTP_GET, index_js_handler);
    register_uri(handle, "/assets/index2.js", HTTP_GET, index2_js_handler);
    register_uri(handle, "/assets/exif-reader.js", HTTP_GET, exif_reader_js_handler);
    register_uri(handle, "/assets/browser.js", HTTP_GET, browser_js_handler);
    register_uri(handle, "/assets/__vite-browser-external.js", HTTP_GET,
                 vite_browser_external_js_handler);
    register_uri(handle, "/icon.svg", HTTP_GET, icon_handler);
    register_uri(handle, "/profile-editor.html", HTTP_GET, profile_editor_handler);
    register_uri(handle, "/measurement_sample.jpg", HTTP_GET, measurement_sample_handler);
    register_uri(handle, "/api/rotate", HTTP_POST, rotate_handler);
    register_uri(handle, "/api/current_image", HTTP_GET, current_image_handler);
    register_uri(handle, "/api/config", HTTP_GET, config_handler);
    register_uri(handle, "/api/config", HTTP_POST, config_handler);
    register_uri(handle, "/api/config", HTTP_PATCH, config_handler);
    register_uri(handle, "/api/config/urls", HTTP_GET, config_urls_handler);
    register_uri(handle, "/api/debug/log", HTTP_GET, debug_log_download_handler);
    register_uri(handle, "/api/debug/log", HTTP_DELETE, debug_log_clear_handler);
    register_uri(handle, "/api/battery", HTTP_GET, battery_handler);
    register_uri(handle, "/api/battery-history", HTTP_GET, battery_history_handler);
    register_uri(handle, "/api/battery-history", HTTP_DELETE, battery_history_handler);
    register_uri(handle, "/api/history", HTTP_GET, display_history_handler);
    register_uri(handle, "/api/history", HTTP_DELETE, display_history_handler);
    register_uri(handle, "/api/albums/organize-crop", HTTP_POST, organize_crop_variants_handler);
    register_uri(handle, "/api/sensor", HTTP_GET, sensor_handler);
    register_uri(handle, "/api/climate-history", HTTP_GET, climate_history_handler);
    register_uri(handle, "/api/climate-history", HTTP_DELETE, climate_history_handler);
    register_uri(handle, "/api/sleep", HTTP_POST, sleep_handler);
    register_uri(handle, "/api/system-info", HTTP_GET, system_info_handler);
    register_uri(handle, "/api/time", HTTP_GET, time_handler);
    register_uri(handle, "/api/time/sync", HTTP_POST, time_sync_handler);
    register_uri(handle, "/api/ota/status", HTTP_GET, ota_status_handler);
    register_uri(handle, "/api/ota/check", HTTP_POST, ota_check_handler);
    register_uri(handle, "/api/ota/update", HTTP_POST, ota_update_handler);
    register_uri(handle, "/api/keep_alive", HTTP_POST, keep_alive_handler);
    register_uri(handle, "/api/format-storage", HTTP_POST, format_storage_handler);
    register_uri(handle, "/api/display-image", HTTP_POST, display_image_direct_handler);
    register_uri(handle, "/api/albums", HTTP_GET, albums_handler);
    register_uri(handle, "/api/albums", HTTP_POST, albums_handler);
    register_uri(handle, "/api/albums", HTTP_DELETE, album_delete_handler);
    register_uri(handle, "/api/albums/enabled", HTTP_PUT, album_enabled_handler);
    register_uri(handle, "/api/images", HTTP_GET, album_images_handler);
    register_uri(handle, "/api/upload", HTTP_POST, upload_image_handler);
    register_uri(handle, "/api/display", HTTP_POST, display_image_handler);
    register_uri(handle, "/api/delete", HTTP_POST, delete_image_handler);
    register_uri(handle, "/api/image", HTTP_GET, serve_image_handler);
    register_uri(handle, "/api/settings/processing", HTTP_GET, processing_settings_handler);
    register_uri(handle, "/api/settings/processing", HTTP_POST, processing_settings_handler);
    register_uri(handle, "/api/settings/processing", HTTP_DELETE, processing_settings_handler);
    register_uri(handle, "/api/settings/palette", HTTP_GET, color_palette_handler);
    register_uri(handle, "/api/settings/palette", HTTP_POST, color_palette_handler);
    register_uri(handle, "/api/settings/palette", HTTP_DELETE, color_palette_handler);
    register_uri(handle, "/api/factory-reset", HTTP_POST, factory_reset_handler);
    register_uri(handle, "/api/calibration/display", HTTP_POST, display_calibration_handler);
    register_uri(handle, "/api/error-overlay/test", HTTP_POST, error_overlay_test_handler);
    register_uri(handle, "/api/chimes/test", HTTP_POST, chime_test_handler);
    register_uri(handle, "/api/agenda/extra-ics", HTTP_POST, agenda_extra_ics_upload_handler);
    register_uri(handle, "/api/agenda/color-profile", HTTP_GET, agenda_color_profile_handler);
    register_uri(handle, "/api/agenda/color-profile", HTTP_POST, agenda_color_profile_handler);
    register_uri(handle, "/api/agenda/color-profile", HTTP_DELETE, agenda_color_profile_handler);
    register_uri(handle, "/api/wifi/hotspot/start", HTTP_POST, wifi_hotspot_start_handler);
    register_uri(handle, "/api/wifi/hotspot/stop", HTTP_POST, wifi_hotspot_stop_handler);
}

esp_err_t http_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // 72: 62 handlers are registered below as of this comment (the offline
    // hotspot start/stop endpoints pushed the previous 64-handler margin -
    // itself already raised twice before, from 50 then 55 - down to 2 free
    // slots). Keep real margin above the exact count so the next handler
    // added here doesn't silently fail to register
    // (httpd_register_uri_handler() only logs a warning on overflow, never a
    // hard error, and every following handler in the same init function
    // still gets registered fine - only the ones actually over the limit
    // silently vanish, which is what made this so easy to miss before -
    // run `grep -c "register_uri(handle," main/http_server.c` and compare
    // against this number whenever you add a new endpoint).
    config.max_uri_handlers = 72;
    // 16384: rotate_handler() (/api/rotate) calls trigger_image_rotation()
    // synchronously on this worker task - the same heavy pipeline that's
    // needed the same bump on button_task/deep_sleep_wake_task (12288 wasn't
    // enough there either, confirmed by a live coredump - see main.c).
    config.stack_size = 16384;
    config.max_open_sockets = 10;    // Limit concurrent connections to prevent memory exhaustion
    config.lru_purge_enable = true;  // Enable LRU purging of connections

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    register_all_handlers(server);
    ESP_LOGI(TAG, "HTTP server started");

    // Optional second HTTPS instance (github.com/aitjcize/esp32-photoframe#130)
    // on the same routes, port 443 - off by default (self-signed cert, so
    // every client sees a browser warning to click through) via
    // config_manager_get_https_enabled(). Purely additive: the plain HTTP
    // instance above is never disabled by this, so existing bookmarks,
    // Home Assistant, and any other scripted client keep working
    // unchanged either way.
    if (config_manager_get_https_enabled()) {
        const uint8_t *cert_der, *key_der;
        size_t cert_len, key_len;
        if (https_cert_get(&cert_der, &cert_len, &key_der, &key_len) == ESP_OK) {
            httpd_ssl_config_t https_config = HTTPD_SSL_CONFIG_DEFAULT();
            https_config.httpd.max_uri_handlers = 72;
            https_config.httpd.stack_size = 16384;
            https_config.httpd.max_open_sockets =
                4;  // TLS sockets cost real RAM - see esp_https_server.h
            https_config.httpd.lru_purge_enable = true;
            https_config.servercert = cert_der;
            https_config.servercert_len = cert_len;
            https_config.prvtkey_pem = key_der;
            https_config.prvtkey_len = key_len;

            if (httpd_ssl_start(&https_server, &https_config) == ESP_OK) {
                register_all_handlers(https_server);
                ESP_LOGI(TAG, "HTTPS server started on port %d (self-signed certificate)",
                         https_config.port_secure);
            } else {
                ESP_LOGE(TAG, "Failed to start HTTPS server - continuing with HTTP only");
            }
        } else {
            ESP_LOGE(TAG, "Failed to obtain HTTPS certificate - continuing with HTTP only");
        }
    }

    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (https_server) {
        httpd_ssl_stop(https_server);
        https_server = NULL;
        ESP_LOGI(TAG, "HTTPS server stopped");
    }
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ESP_OK;
}

void http_server_set_ready(void)
{
    system_ready = true;
    ESP_LOGI(TAG, "System marked as ready for HTTP requests");
}
