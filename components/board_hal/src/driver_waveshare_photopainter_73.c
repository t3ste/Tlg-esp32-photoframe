#include <time.h>

#include "axp2101.h"
#include "board_hal.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "pcf85063.h"
#include "sensor.h"

#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif

static const char *TAG = "board_hal_waveshare";

static i2c_master_bus_handle_t i2c_bus = NULL;

esp_err_t board_hal_init(void)
{
    // Initialize I2C bus
    ESP_LOGI(TAG, "Initializing I2C bus...");
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .scl_io_num = 48,
        .sda_io_num = 47,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    ESP_LOGI(TAG, "Initializing WaveShare PhotoPainter Power HAL");
    axp2101_init(i2c_bus);
    axp2101_cmd_init();

    // Initialize SHTC3 sensor (part of this board's power/sensor hal)
    esp_err_t shtc3_ret = sensor_init(i2c_bus);
    if (shtc3_ret == ESP_OK) {
        ESP_LOGI(TAG, "SHTC3 sensor initialized successfully");
    } else {
        ESP_LOGW(TAG, "SHTC3 sensor initialization failed (sensor may not be present)");
    }

    // Initialize SD Card (SDIO)
    sdcard_config_t sd_cfg = {
        .clk_pin = BOARD_HAL_SD_CLK_PIN,
        .cmd_pin = BOARD_HAL_SD_CMD_PIN,
        .d0_pin = BOARD_HAL_SD_D0_PIN,
        .d1_pin = BOARD_HAL_SD_D1_PIN,
        .d2_pin = BOARD_HAL_SD_D2_PIN,
        .d3_pin = BOARD_HAL_SD_D3_PIN,
    };
    if (sdcard_init(&sd_cfg) == ESP_OK) {
        ESP_LOGI(TAG, "SD Card initialized");
    } else {
        ESP_LOGE(TAG, "Failed to initialize SD Card");
    }

    // Initialize SPI bus
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1200 * 825 / 2 + 100,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Initialize E-Paper Display Port
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_cs1 = -1,
        .pin_enable = -1,
    };
    epaper_init(&ep_cfg);

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing system for sleep");

    // Put SHTC3 sensor to sleep
    if (sensor_is_available()) {
        sensor_sleep();
        ESP_LOGI(TAG, "SHTC3 sensor put to sleep");
    }

    ESP_LOGI(TAG, "Preparing AXP2101 for sleep");
    axp2101_basic_sleep_start();
    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    return axp2101_is_battery_connected();
}

int board_hal_get_battery_percent(void)
{
    return axp2101_get_battery_percent();
}

int board_hal_get_battery_voltage(void)
{
    return axp2101_get_battery_voltage();
}

bool board_hal_is_charging(void)
{
    return axp2101_is_charging();
}

bool board_hal_is_usb_connected(void)
{
    return axp2101_is_usb_connected();
}

void board_hal_shutdown(void)
{
    axp2101_shutdown();
}

esp_err_t board_hal_get_temperature(float *t)
{
    if (!t)
        return ESP_ERR_INVALID_ARG;
    if (!sensor_is_available())
        return ESP_ERR_INVALID_STATE;

    float h_dummy;
    return sensor_read(t, &h_dummy);
}

esp_err_t board_hal_get_humidity(float *h)
{
    if (!h)
        return ESP_ERR_INVALID_ARG;
    if (!sensor_is_available())
        return ESP_ERR_INVALID_STATE;
    float t_dummy;
    return sensor_read(&t_dummy, h);
}

esp_err_t board_hal_rtc_init(void)
{
    return pcf85063_init(i2c_bus);
}

esp_err_t board_hal_rtc_get_time(time_t *t)
{
    if (!t)
        return ESP_ERR_INVALID_ARG;
    return pcf85063_read_time(t);
}

esp_err_t board_hal_rtc_set_time(time_t t)
{
    return pcf85063_write_time(t);
}

bool board_hal_rtc_is_available(void)
{
    return pcf85063_is_available();
}
