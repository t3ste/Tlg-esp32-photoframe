#include "improv_serial.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv.h"

extern "C" {
#include "wifi_manager.h"
#include "wifi_provisioning.h"
}

static const char *TAG = "improv_serial";

#define UART_NUM UART_NUM_0
#define BUF_SIZE 1024
#define RX_BUF_SIZE (BUF_SIZE * 2)

static TaskHandle_t improv_task_handle = NULL;
static bool is_running = false;
static char redirect_url[128] = "http://photoframe.local";

// Improv Serial protocol state
static uint8_t rx_buffer[256];
static size_t rx_position = 0;

static void send_response(improv::Command command, const std::vector<std::string> &data)
{
    std::vector<uint8_t> response = improv::build_rpc_response(command, data, true);
    uart_write_bytes(UART_NUM, (const char *) response.data(), response.size());
}

static void send_state(improv::State state)
{
    uint8_t data[4];
    data[0] = 'I';
    data[1] = 'M';
    data[2] = improv::TYPE_CURRENT_STATE;
    data[3] = state;
    uart_write_bytes(UART_NUM, (const char *) data, 4);
}

static void send_error(improv::Error error)
{
    uint8_t data[4];
    data[0] = 'I';
    data[1] = 'M';
    data[2] = improv::TYPE_ERROR_STATE;
    data[3] = error;
    uart_write_bytes(UART_NUM, (const char *) data, 4);
}

static void handle_command(const improv::ImprovCommand &cmd)
{
    switch (cmd.command) {
    case improv::WIFI_SETTINGS: {
        ESP_LOGI(TAG, "Received WiFi credentials - SSID: %s", cmd.ssid.c_str());

        send_state(improv::STATE_PROVISIONING);

        // Save and connect to WiFi
        esp_err_t err = wifi_manager_save_credentials(cmd.ssid.c_str(), cmd.password.c_str());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save WiFi credentials");
            send_error(improv::ERROR_UNABLE_TO_CONNECT);
            send_state(improv::STATE_AUTHORIZED);
            return;
        }

        // Wait for connection
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "WiFi connection successful!");
            send_state(improv::STATE_PROVISIONED);

            // Send success response with redirect URL
            std::vector<std::string> urls = {std::string(redirect_url)};
            send_response(improv::WIFI_SETTINGS, urls);
        } else {
            ESP_LOGE(TAG, "WiFi connection failed");
            send_error(improv::ERROR_UNABLE_TO_CONNECT);
            send_state(improv::STATE_AUTHORIZED);
        }
        break;
    }

    case improv::GET_CURRENT_STATE:
        if (wifi_provisioning_is_provisioned()) {
            send_state(improv::STATE_PROVISIONED);
        } else {
            send_state(improv::STATE_AUTHORIZED);
        }
        break;

    case improv::GET_DEVICE_INFO: {
        std::vector<std::string> info = {
            "PhotoFrame",           // Firmware name
#ifdef FIRMWARE_VERSION
            FIRMWARE_VERSION,
#else
            "dev",
#endif
            "ESP32-S3",            // Hardware chip/variant
            "PhotoFrame Control"   // Device name
        };
        send_response(improv::GET_DEVICE_INFO, info);
        break;
    }

    case improv::GET_WIFI_NETWORKS:
        // Not implemented - would require WiFi scan
        ESP_LOGW(TAG, "WiFi scan not implemented");
        break;

    default:
        ESP_LOGW(TAG, "Unknown command: %d", cmd.command);
        send_error(improv::ERROR_UNKNOWN_RPC);
        break;
    }
}

static void improv_serial_task(void *arg)
{
    uint8_t data[BUF_SIZE];

    ESP_LOGI(TAG, "Improv Serial task started");

    // Send initial state
    if (wifi_provisioning_is_provisioned()) {
        send_state(improv::STATE_PROVISIONED);
    } else {
        send_state(improv::STATE_AUTHORIZED);
    }

    while (is_running) {
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE, pdMS_TO_TICKS(100));

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                bool result = improv::parse_improv_serial_byte(
                    rx_position, data[i], rx_buffer,
                    [](improv::ImprovCommand cmd) -> bool {
                        handle_command(cmd);
                        return true;
                    },
                    [](improv::Error error) { send_error(error); });

                if (result) {
                    rx_position = 0;
                } else {
                    rx_position++;
                    if (rx_position >= sizeof(rx_buffer)) {
                        rx_position = 0;
                    }
                }
            }
        }
    }

    ESP_LOGI(TAG, "Improv Serial task stopped");
    improv_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t improv_serial_init(void)
{
    ESP_LOGI(TAG, "Improv Serial initialized");
    return ESP_OK;
}

esp_err_t improv_serial_start(void)
{
    if (is_running) {
        ESP_LOGW(TAG, "Improv Serial already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting Improv Serial service");

    // UART is already configured by ESP-IDF, just start the task
    is_running = true;
    rx_position = 0;

    BaseType_t ret = xTaskCreate(improv_serial_task, "improv_serial", 4096, NULL, 5,
                                  &improv_task_handle);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Improv Serial task");
        is_running = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t improv_serial_stop(void)
{
    if (!is_running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping Improv Serial service");
    is_running = false;

    // Wait for task to finish
    int timeout = 50;
    while (improv_task_handle != NULL && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}

bool improv_serial_is_running(void)
{
    return is_running;
}

void improv_serial_set_redirect_url(const char *url)
{
    if (url) {
        strncpy(redirect_url, url, sizeof(redirect_url) - 1);
        redirect_url[sizeof(redirect_url) - 1] = '\0';
    }
}
