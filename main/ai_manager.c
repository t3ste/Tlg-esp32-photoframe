#include "ai_manager.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "image_processor.h"
#include "mbedtls/base64.h"
#include "processing_settings.h"
#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif
#include "display_manager.h"
#include "wifi_manager.h"

static const char *TAG = "ai_manager";

// Embedded OpenAI Root Certificate
extern const uint8_t openai_root_pem_start[] asm("_binary_openai_root_pem_start");
extern const uint8_t openai_root_pem_end[] asm("_binary_openai_root_pem_end");

static ai_generation_status_t current_status = AI_STATUS_IDLE;
static char last_image_path[128] = {0};
static char last_error[256] = {0};
static char current_prompt[AI_PROMPT_MAX_LEN] = {0};
static char current_model[64] = {0};

static TaskHandle_t ai_task_handle = NULL;
static SemaphoreHandle_t generation_trigger = NULL;

static void ai_task(void *pvParameters);
static esp_err_t generate_openai_request(const char *prompt, const char *model,
                                         char **response_out);
static esp_err_t download_image_to_buffer(const char *url, uint8_t **out_buffer, size_t *out_size);
static esp_err_t decode_b64_to_buffer(const char *b64_str, uint8_t **out_buffer, size_t *out_size);

esp_err_t ai_manager_init(void)
{
    generation_trigger = xSemaphoreCreateBinary();
    if (generation_trigger == NULL) {
        ESP_LOGE(TAG, "Failed to create generation semaphore");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreate(ai_task, "ai_task", 8192, NULL, 5, &ai_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AI task");
        vSemaphoreDelete(generation_trigger);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AI Manager initialized");
    return ESP_OK;
}

esp_err_t ai_manager_generate_and_display(const char *prompt_override)
{
    if (current_status != AI_STATUS_IDLE && current_status != AI_STATUS_COMPLETE &&
        current_status != AI_STATUS_ERROR) {
        ESP_LOGW(TAG, "Generation already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Determine prompt
    if (prompt_override && strlen(prompt_override) > 0) {
        strncpy(current_prompt, prompt_override, sizeof(current_prompt) - 1);
    } else {
        const char *config_prompt = config_manager_get_ai_prompt();
        if (config_prompt && strlen(config_prompt) > 0) {
            strncpy(current_prompt, config_prompt, sizeof(current_prompt) - 1);
        } else {
            strncpy(current_prompt, "A random artistic image", sizeof(current_prompt) - 1);
        }
    }
    current_prompt[sizeof(current_prompt) - 1] = '\0';

    // Get model from config
    const char *config_model = config_manager_get_ai_model();
    strncpy(current_model, config_model, sizeof(current_model) - 1);
    current_model[sizeof(current_model) - 1] = '\0';

    current_status = AI_STATUS_IDLE;  // Reset status
    xSemaphoreGive(generation_trigger);
    return ESP_OK;
}

// Helper to parse OpenAI response and get image data
static esp_err_t parse_openai_json_to_image(const char *json_buf, uint8_t **out_buffer,
                                            size_t *out_size)
{
    cJSON *resp_json = cJSON_Parse(json_buf);
    if (!resp_json)
        return ESP_FAIL;

    esp_err_t err = ESP_FAIL;
    cJSON *data = cJSON_GetObjectItem(resp_json, "data");
    cJSON *first = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;

    if (first) {
        cJSON *url = cJSON_GetObjectItem(first, "url");
        cJSON *b64 = cJSON_GetObjectItem(first, "b64_json");

        if (cJSON_IsString(url) && url->valuestring) {
            current_status = AI_STATUS_DOWNLOADING;
            err = download_image_to_buffer(url->valuestring, out_buffer, out_size);
        } else if (cJSON_IsString(b64) && b64->valuestring) {
            current_status = AI_STATUS_DOWNLOADING;
            err = decode_b64_to_buffer(b64->valuestring, out_buffer, out_size);
        }
    }

    cJSON_Delete(resp_json);
    return err;
}

static void ai_task(void *pvParameters)
{
    while (1) {
        if (xSemaphoreTake(generation_trigger, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Starting AI generation with prompt: %s", current_prompt);
            current_status = AI_STATUS_GENERATING;
            last_error[0] = '\0';

            // Make OpenAI API request
            char *json_response = NULL;
            esp_err_t err = generate_openai_request(current_prompt, current_model, &json_response);

            if (err != ESP_OK || !json_response) {
                ESP_LOGE(TAG, "Generation failed");
                if (last_error[0] == '\0') {
                    snprintf(last_error, sizeof(last_error), "API request failed");
                }
                current_status = AI_STATUS_ERROR;
                continue;
            }

            // Parse response and get image data
            uint8_t *image_buffer = NULL;
            size_t image_size = 0;
            err = parse_openai_json_to_image(json_response, &image_buffer, &image_size);
            heap_caps_free(json_response);

            if (err != ESP_OK || !image_buffer) {
                ESP_LOGE(TAG, "Failed to get image from response");
                if (last_error[0] == '\0') {
                    snprintf(last_error, sizeof(last_error), "Failed to download image");
                }
                current_status = AI_STATUS_ERROR;
                continue;
            }

            ESP_LOGI(TAG, "Got image data: %zu bytes, processing...", image_size);

            processing_settings_t settings;
            if (processing_settings_load(&settings) != ESP_OK) {
                processing_settings_get_defaults(&settings);
            }
            dither_algorithm_t algo = processing_settings_get_dithering_algorithm();

            // Save thumbnail for /api/current_image
            FILE *thumb_fp = fopen(CURRENT_JPG_PATH, "wb");
            if (thumb_fp) {
                fwrite(image_buffer, 1, image_size, thumb_fp);
                fclose(thumb_fp);
                ESP_LOGI(TAG, "Saved thumbnail: %s", CURRENT_JPG_PATH);
            }

#ifdef CONFIG_HAS_SDCARD
            // SD card: use file-based processing, save processed PNG
            char output_path[256];
            bool save_to_downloads =
                config_manager_get_save_downloaded_images() && sdcard_is_mounted();

            if (save_to_downloads) {
                time_t now;
                time(&now);
                struct tm timeinfo;
                localtime_r(&now, &timeinfo);

                struct stat st;
                if (stat(DOWNLOAD_DIRECTORY, &st) != 0)
                    mkdir(DOWNLOAD_DIRECTORY, 0755);

                snprintf(output_path, sizeof(output_path),
                         DOWNLOAD_DIRECTORY "/ai_%04d%02d%02d_%02d%02d%02d.png",
                         timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                         timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            } else {
                strncpy(output_path, CURRENT_PNG_PATH, sizeof(output_path) - 1);
            }

            err = image_processor_process(CURRENT_JPG_PATH, output_path, algo);
            heap_caps_free(image_buffer);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Image processing failed");
                snprintf(last_error, sizeof(last_error), "Processing failed");
                current_status = AI_STATUS_ERROR;
                continue;
            }

            ESP_LOGI(TAG, "Image processed to: %s", output_path);

            err = display_manager_show_image(output_path);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Image displayed successfully");
                strncpy(last_image_path, output_path, sizeof(last_image_path) - 1);
                current_status = AI_STATUS_COMPLETE;
            } else {
                ESP_LOGE(TAG, "Failed to display image");
                snprintf(last_error, sizeof(last_error), "Display failed");
                current_status = AI_STATUS_ERROR;
            }
#else
            // SD-card-less: process to RGB buffer and display directly
            image_process_rgb_result_t result;
            err = image_processor_process_to_rgb(image_buffer, image_size, IMAGE_FORMAT_JPG, algo,
                                                 &result);
            heap_caps_free(image_buffer);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Image processing failed");
                snprintf(last_error, sizeof(last_error), "Processing failed");
                current_status = AI_STATUS_ERROR;
                continue;
            }

            ESP_LOGI(TAG, "Image processed to RGB: %dx%d", result.width, result.height);

            err = display_manager_show_rgb_buffer(result.rgb_data, result.width, result.height);
            heap_caps_free(result.rgb_data);

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Image displayed successfully");
                strncpy(last_image_path, CURRENT_JPG_PATH, sizeof(last_image_path) - 1);
                current_status = AI_STATUS_COMPLETE;
            } else {
                ESP_LOGE(TAG, "Failed to display image");
                snprintf(last_error, sizeof(last_error), "Display failed");
                current_status = AI_STATUS_ERROR;
            }
#endif
        }
    }
}

// Decode base64 to buffer (used for both SD card and SD-card-less systems)
static esp_err_t decode_b64_to_buffer(const char *b64_str, uint8_t **out_buffer, size_t *out_size)
{
    size_t b64_len = strlen(b64_str);
    size_t decoded_len = 0;

    // Allocate buffer for decoded data (base64 decodes to ~75% of input size)
    uint8_t *decoded_data = heap_caps_malloc(b64_len, MALLOC_CAP_SPIRAM);
    if (!decoded_data)
        return ESP_ERR_NO_MEM;

    if (mbedtls_base64_decode(decoded_data, b64_len, &decoded_len, (const unsigned char *) b64_str,
                              b64_len) != 0) {
        heap_caps_free(decoded_data);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Decoded %zu bytes from base64", decoded_len);
    *out_buffer = decoded_data;
    *out_size = decoded_len;
    return ESP_OK;
}

static esp_err_t download_image_to_buffer(const char *url, uint8_t **out_buffer, size_t *out_size)
{
    esp_http_client_config_t config = {
        .url = url,
        .skip_cert_common_name_check = true,
        .timeout_ms = 60000,
        .buffer_size = 16384,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK || esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_get_content_length(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid content length");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t *buffer = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for download", content_length);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    while (total_read < content_length) {
        int read_len =
            esp_http_client_read(client, (char *) buffer + total_read, content_length - total_read);
        if (read_len <= 0)
            break;
        total_read += read_len;
    }

    esp_http_client_cleanup(client);

    if (total_read != content_length) {
        ESP_LOGE(TAG, "Download incomplete: %d/%d bytes", total_read, content_length);
        heap_caps_free(buffer);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes to buffer", total_read);
    *out_buffer = buffer;
    *out_size = total_read;
    return ESP_OK;
}

// Shared function to make OpenAI API request and get JSON response
// Returns allocated response buffer that caller must free with heap_caps_free
static esp_err_t generate_openai_request(const char *prompt, const char *model, char **response_out)
{
    *response_out = NULL;
    const char *api_key = config_manager_get_openai_api_key();
    if (!api_key || strlen(api_key) == 0) {
        snprintf(last_error, sizeof(last_error), "API Key missing");
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = "https://api.openai.com/v1/images/generations",
        .method = HTTP_METHOD_POST,
        .cert_pem = (const char *) openai_root_pem_start,
        .timeout_ms = 90000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
        return ESP_FAIL;

    esp_err_t ret = ESP_FAIL;
    char *json_str = NULL;
    cJSON *root = NULL;

    char auth_header[AI_API_KEY_MAX_LEN + 16];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddStringToObject(root, "prompt", prompt);
    cJSON_AddNumberToObject(root, "n", 1);

    display_orientation_t orient = config_manager_get_display_orientation();
    cJSON_AddStringToObject(root, "size",
                            orient == DISPLAY_ORIENTATION_PORTRAIT ? "1024x1536" : "1536x1024");
    cJSON_AddStringToObject(root, "quality", "high");
    cJSON_AddStringToObject(root, "output_format", "jpeg");
    cJSON_AddNumberToObject(root, "output_compression", 90);

    json_str = cJSON_PrintUnformatted(root);
    if (!json_str)
        goto cleanup;

    if (esp_http_client_open(client, strlen(json_str)) != ESP_OK) {
        goto cleanup;
    }

    if (esp_http_client_write(client, json_str, strlen(json_str)) < 0) {
        goto cleanup;
    }

    esp_http_client_fetch_headers(client);

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        snprintf(last_error, sizeof(last_error), "API Error: %d", status_code);
        goto cleanup;
    }

    const size_t resp_size = 2 * 1024 * 1024;
    char *response_buf = heap_caps_malloc(resp_size, MALLOC_CAP_SPIRAM);
    if (response_buf) {
        int total_read = 0;
        while (total_read < resp_size - 1) {
            int read_len =
                esp_http_client_read(client, response_buf + total_read, resp_size - total_read - 1);
            if (read_len <= 0)
                break;
            total_read += read_len;
        }
        response_buf[total_read] = '\0';
        if (total_read > 0) {
            *response_out = response_buf;
            ret = ESP_OK;
        } else {
            heap_caps_free(response_buf);
        }
    }

cleanup:
    if (root)
        cJSON_Delete(root);
    if (json_str)
        free(json_str);
    esp_http_client_cleanup(client);
    return ret;
}

ai_generation_status_t ai_manager_get_status(void)
{
    return current_status;
}

const char *ai_manager_get_last_image_path(void)
{
    return last_image_path;
}

const char *ai_manager_get_last_error(void)
{
    return last_error;
}