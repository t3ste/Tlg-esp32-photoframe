#include "board_hal.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#ifndef CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73

bool board_hal_has_speaker(void)
{
    return false;
}

esp_err_t board_hal_play_beep_pattern(board_hal_chime_kind_t kind, uint8_t volume_percent)
{
    (void) kind;
    (void) volume_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

#else

#include <math.h>
#include <string.h>

#include "axp2101.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "board_audio";

#define CHIME_SAMPLE_RATE 16000
#define CHIME_AMPLITUDE 7000

// ES8311 register map - live-verified byte-for-byte against a known-good
// register dump pulled from Waveshare's own shipped Arduino example
// (05_ArduinoExample/01_Audio_Test, esp_codec_dev's es8311.c) running on
// identical hardware. Two earlier attempts based on a generic/simplified
// ES8311 bring-up sequence (missing several registers below, plus an
// invented reset pulse not present in the real code) produced a fully
// "successful" I2C/I2S bring-up with zero audible output - so every value
// here is taken from the real trace, not derived from the datasheet alone.
#define ES8311_REG_RESET 0x00
#define ES8311_REG_CLK_MANAGER1 0x01
#define ES8311_REG_CLK_MANAGER2 0x02
#define ES8311_REG_CLK_MANAGER3 0x03
#define ES8311_REG_CLK_MANAGER4 0x04
#define ES8311_REG_CLK_MANAGER5 0x05
#define ES8311_REG_CLK_MANAGER6 0x06  // bclk divider
#define ES8311_REG_CLK_MANAGER7 0x07  // lrck divider, high bits
#define ES8311_REG_CLK_MANAGER8 0x08  // lrck divider, low bits
#define ES8311_REG_SDP_IN 0x09
#define ES8311_REG_SDP_OUT 0x0A
#define ES8311_REG_SYSTEM_0B 0x0B
#define ES8311_REG_SYSTEM_0C 0x0C
#define ES8311_REG_SYSTEM1 0x0D
#define ES8311_REG_SYSTEM2 \
    0x0E  // analog output power up/down - stays
          // powered down at reset until this is set;
          // the single register that mattered most
          // across the earlier failed attempts
#define ES8311_REG_SYSTEM3 0x12
#define ES8311_REG_SYSTEM4 0x13
#define ES8311_REG_SYSTEM5 0x14  // DMIC select / analog PGA gain
#define ES8311_REG_SYSTEM_10 0x10
#define ES8311_REG_SYSTEM_11 0x11
#define ES8311_REG_ADC_15 0x15
#define ES8311_REG_ADC_17 0x17
#define ES8311_REG_SYSTEM7 0x1B
#define ES8311_REG_SYSTEM8 0x1C
#define ES8311_REG_DAC_MUTE \
    0x31  // separate from DAC_VOL below - the actual
          // hardware mute flag (bits 0x60); volume
          // alone is silent until this is cleared
#define ES8311_REG_DAC_VOL 0x32
#define ES8311_REG_DAC_RAMPRATE 0x37
#define ES8311_REG_GPIO 0x44  // internal reference signal routing (ADCL+DACR)
#define ES8311_REG_GP_CONTROL 0x45
#define ES8311_REG_CLK_DIV 0x16
#define ES8311_REG_CHIP_ID1 0xFD
#define ES8311_CHIP_ID 0x83

// This device's I2C bus is shared (AXP2101/RTC/SHTC3, see
// board_hal_get_i2c_bus()) and every play opens/closes its own I2S+codec
// session (see audio_session_open/_close below) rather than keeping one
// running - a mutex just serializes concurrent play attempts, it isn't
// protecting shared hardware state across calls.
static SemaphoreHandle_t s_chime_mutex;

static void chime_mutex_init(void)
{
    if (!s_chime_mutex) {
        s_chime_mutex = xSemaphoreCreateMutex();
    }
}

static esp_err_t es8311_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t es8311_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static void pa_set(bool enable)
{
    // NS4150B CTRL is active-high - confirmed against the board schematic's
    // "AudioCTR" net, which traces from this GPIO straight to the amp's
    // CTRL pin.
    gpio_set_level(BOARD_HAL_AUDIO_PA_PIN, enable ? 1 : 0);
}

// ES8311 bring-up for 16-bit/16kHz DAC-only playback, MCLK-driven, ESP32 as
// I2S master (codec as slave). Mirrors the real open()+set_fs()+enable()
// call chain in order - unlike a generic simplified sequence, several of
// these registers are only correct because of what ran immediately before
// them (a few are deliberately re-read-and-modified rather than written
// outright, matching the original code's own read-modify-write pattern).
static esp_err_t es8311_dac_init(i2c_master_dev_handle_t dev, uint8_t volume_percent)
{
    uint8_t chip_id = 0;
    esp_err_t err = es8311_read(dev, ES8311_REG_CHIP_ID1, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 not responding on I2C 0x%02x", BOARD_HAL_AUDIO_ES8311_ADDR);
        return err;
    }
    if (chip_id != ES8311_CHIP_ID) {
        ESP_LOGW(TAG, "Unexpected ES8311 chip ID 0x%02x (expected 0x%02x)", chip_id,
                 ES8311_CHIP_ID);
    }

    // ---- one-time bring-up ----
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_GPIO, 0x08));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        es8311_write(dev, ES8311_REG_GPIO, 0x08));  // written twice for I2C noise immunity

    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER1, 0x30));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER2, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER3, 0x10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_DIV, 0x24));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER4, 0x10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER5, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM_0B, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM_0C, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM_10, 0x1F));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM_11, 0x7F));

    // The only write to the reset register: a single 0x80, at this exact
    // point - not first, and not preceded by any other reset pulse.
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_RESET, 0x80));

    // Re-write CLK_MANAGER1 with the final (use_mclk=true) value - overwrites
    // the 0x30 above.
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER1, 0x3F));
    // Slave mode (ESP32 is I2S master): clear bit 0x20 on REG06 relative to
    // its true chip-reset value.
    uint8_t reg06 = 0;
    if (es8311_read(dev, ES8311_REG_CLK_MANAGER6, &reg06) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            es8311_write(dev, ES8311_REG_CLK_MANAGER6, reg06 & (uint8_t) ~0x20));
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM4, 0x10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM7, 0x0A));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM8, 0x6A));
    // Internal reference signal routing (ADCL+DACR).
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_GPIO, 0x58));

    // ---- format/sample-rate ----
    uint8_t sdp_in = 0, sdp_out = 0;
    es8311_read(dev, ES8311_REG_SDP_IN, &sdp_in);
    es8311_read(dev, ES8311_REG_SDP_OUT, &sdp_out);
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SDP_IN, sdp_in | 0x0C));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SDP_OUT, sdp_out | 0x0C));
    es8311_read(dev, ES8311_REG_SDP_IN, &sdp_in);
    es8311_read(dev, ES8311_REG_SDP_OUT, &sdp_out);
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SDP_IN, sdp_in & 0xFC));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SDP_OUT, sdp_out & 0xFC));
    // Coefficients for {mclk: 4096000, rate: 16000} (256x MCLK multiple):
    // pre_div=1, pre_multi=1, adc_div=1, dac_div=1, fs_mode=0, lrck_h=0,
    // lrck_l=0xff, bclk_div=4, adc_osr=0x10, dac_osr=0x20.
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER2, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER5, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER3, 0x10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER4, 0x20));
    uint8_t reg07 = 0;
    es8311_read(dev, ES8311_REG_CLK_MANAGER7, &reg07);
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER7, reg07 & 0xC0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_CLK_MANAGER8, 0xFF));
    es8311_read(dev, ES8311_REG_CLK_MANAGER6, &reg06);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        es8311_write(dev, ES8311_REG_CLK_MANAGER6, (reg06 & 0xE0) | 0x03));

    // ---- enable ----
    es8311_read(dev, ES8311_REG_SDP_IN, &sdp_in);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        es8311_write(dev, ES8311_REG_SDP_IN, (uint8_t) (sdp_in & 0xBF)));  // DAC mode: bit6 clear
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_ADC_17, 0xBF));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM2, 0x02));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM3, 0x00));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM5, 0x1A));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_SYSTEM1, 0x01));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_ADC_15, 0x40));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_DAC_RAMPRATE, 0x08));
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_GP_CONTROL, 0x00));

    // Linear 0-100% -> 0x00-0xFF. Not perceptually linear (the register is
    // roughly logarithmic, ~0.5dB/step), but simple, monotonic, and good
    // enough for "turn it up/down" - a live-confirmed 0xBF (~75%) played
    // fine, so this range is known-good end to end.
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    uint8_t vol_reg = (uint8_t) ((unsigned) volume_percent * 255 / 100);
    ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_DAC_VOL, vol_reg));

    // Explicit unmute - read-modify-write clearing bits 0x60. Volume alone
    // is not enough; without this the DAC stays hardware-muted regardless
    // of every register above.
    uint8_t mute_reg = 0;
    if (es8311_read(dev, ES8311_REG_DAC_MUTE, &mute_reg) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(es8311_write(dev, ES8311_REG_DAC_MUTE, mute_reg & 0x9F));
    }

    return ESP_OK;
}

// Puts the codec into a known, fully-muted/reset state before the I2C
// device handle is torn down. This board's ES8311 register state survives
// across our open/close cycles (removing the I2C device handle does not
// power-cycle the physical chip), so what this leaves behind is what the
// NEXT es8311_dac_init() call actually starts from.
static void es8311_standby(i2c_master_dev_handle_t dev)
{
    es8311_write(dev, ES8311_REG_DAC_VOL, 0x00);
    es8311_write(dev, ES8311_REG_ADC_17, 0x00);
    es8311_write(dev, ES8311_REG_SYSTEM2, 0xFF);
    es8311_write(dev, ES8311_REG_SYSTEM3, 0x02);
    es8311_write(dev, ES8311_REG_SYSTEM5, 0x00);
    es8311_write(dev, ES8311_REG_SYSTEM1, 0xFA);
    es8311_write(dev, ES8311_REG_ADC_15, 0x00);
    es8311_write(dev, ES8311_REG_CLK_MANAGER2, 0x10);
    es8311_write(dev, ES8311_REG_RESET, 0x00);
    es8311_write(dev, ES8311_REG_RESET, 0x1F);
    es8311_write(dev, ES8311_REG_CLK_MANAGER1, 0x30);
    es8311_write(dev, ES8311_REG_CLK_MANAGER1, 0x00);
    es8311_write(dev, ES8311_REG_GP_CONTROL, 0x00);
    es8311_write(dev, ES8311_REG_SYSTEM1, 0xFC);
    es8311_write(dev, ES8311_REG_CLK_MANAGER2, 0x00);
}

static void i2s_write_silence(i2s_chan_handle_t tx, int frames)
{
    int16_t zeros[128] = {0};
    while (frames > 0) {
        int n = frames > 64 ? 64 : frames;
        size_t written = 0;
        i2s_channel_write(tx, zeros, (size_t) n * 4, &written, pdMS_TO_TICKS(200));
        frames -= n;
    }
}

// Synthesizes and plays one sine-wave tone with an 8ms attack/release
// envelope (avoids an audible click at the start/end of each beep) -
// no WAV/melody data anywhere, every chime is generated on the fly.
static void play_tone(i2s_chan_handle_t tx, float freq_hz, int duration_ms, int amplitude)
{
    const int n = CHIME_SAMPLE_RATE * duration_ms / 1000;
    const int edge = CHIME_SAMPLE_RATE * 8 / 1000;  // 8 ms attack / release
    int16_t buf[256];
    float phase = 0.0f;
    const float phase_inc = 2.0f * (float) M_PI * freq_hz / (float) CHIME_SAMPLE_RATE;
    int produced = 0;

    while (produced < n) {
        int frames = n - produced;
        if (frames > 128) {
            frames = 128;
        }
        for (int i = 0; i < frames; i++) {
            int idx = produced + i;
            float env = 1.0f;
            if (idx < edge) {
                env = (float) idx / (float) edge;
            } else if (idx > n - edge) {
                env = (float) (n - idx) / (float) edge;
            }
            int16_t sample = (int16_t) (sinf(phase) * (float) amplitude * env);
            buf[i * 2] = sample;
            buf[i * 2 + 1] = sample;
            phase += phase_inc;
            if (phase > 2.0f * (float) M_PI) {
                phase -= 2.0f * (float) M_PI;
            }
        }
        size_t written = 0;
        esp_err_t werr =
            i2s_channel_write(tx, buf, (size_t) frames * 4, &written, pdMS_TO_TICKS(500));
        if (werr != ESP_OK || written != (size_t) frames * 4) {
            ESP_LOGE(TAG, "i2s_channel_write failed: %s (wrote %u/%u bytes)", esp_err_to_name(werr),
                     (unsigned) written, (unsigned) (frames * 4));
        }
        produced += frames;
    }
}

// The 3 built-in patterns - one per severity, distinguished by beep count
// (and a lower pitch for the more urgent ones) rather than a chosen melody.
static void play_beep_pattern_tones(i2s_chan_handle_t tx, board_hal_chime_kind_t kind)
{
    int count;
    float freq_hz;
    switch (kind) {
    case BOARD_HAL_CHIME_WARNING:
        count = 2;
        freq_hz = 1200.0f;
        break;
    case BOARD_HAL_CHIME_ERROR:
        count = 3;
        freq_hz = 800.0f;
        break;
    case BOARD_HAL_CHIME_SUCCESS:
    default:
        count = 1;
        freq_hz = 1500.0f;
        break;
    }
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            i2s_write_silence(tx, CHIME_SAMPLE_RATE * 100 / 1000);
        }
        play_tone(tx, freq_hz, 150, CHIME_AMPLITUDE);
    }
}

typedef struct {
    i2s_chan_handle_t tx;
    i2c_master_dev_handle_t es8311;
} audio_session_t;

static void audio_session_close(audio_session_t *s)
{
    pa_set(false);
    if (s->es8311) {
        es8311_standby(s->es8311);
    }
    if (s->tx) {
        i2s_channel_disable(s->tx);
        i2s_del_channel(s->tx);
        s->tx = NULL;
    }
    if (s->es8311) {
        i2c_master_bus_rm_device(s->es8311);
        s->es8311 = NULL;
    }
}

static esp_err_t audio_session_open(audio_session_t *s, uint8_t volume_percent)
{
    memset(s, 0, sizeof(*s));

    axp2101_prepare_audio_rails();
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_config_t pa_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BOARD_HAL_AUDIO_PA_PIN),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_conf);
    pa_set(false);

    i2c_master_bus_handle_t bus = board_hal_get_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_HAL_AUDIO_ES8311_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s->es8311);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8311 I2C device: %s", esp_err_to_name(err));
        return err;
    }

    // Start I2S (and MCLK) before codec register writes - the ES8311 needs a
    // clock present to ack register writes reliably.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &s->tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        audio_session_close(s);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CHIME_SAMPLE_RATE),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = BOARD_HAL_AUDIO_I2S_MCLK_PIN,
                .bclk = BOARD_HAL_AUDIO_I2S_BCLK_PIN,
                .ws = BOARD_HAL_AUDIO_I2S_WS_PIN,
                .dout = BOARD_HAL_AUDIO_I2S_DOUT_PIN,
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(s->tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s->tx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        audio_session_close(s);
        return err;
    }

    err = es8311_dac_init(s->es8311, volume_percent);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 DAC init failed: %s", esp_err_to_name(err));
        audio_session_close(s);
        return err;
    }

    i2s_write_silence(s->tx, 128);
    pa_set(true);
    // NS4150B (or its own soft-start/anti-pop ramp) needs real time to fully
    // turn on after CTRL goes high - confirmed live: a 20s continuous test
    // tone was clearly audible, but a single ~150ms beep with only a 30ms
    // settle delay here was not, on two different physical units. The I2S
    // channel keeps outputting silence during this wait (auto_clear=true
    // above), so nothing is lost by waiting longer.
    vTaskDelay(pdMS_TO_TICKS(250));
    return ESP_OK;
}

bool board_hal_has_speaker(void)
{
    return true;
}

esp_err_t board_hal_play_beep_pattern(board_hal_chime_kind_t kind, uint8_t volume_percent)
{
    chime_mutex_init();
    if (!s_chime_mutex || xSemaphoreTake(s_chime_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    audio_session_t session;
    esp_err_t err = audio_session_open(&session, volume_percent);
    if (err == ESP_OK) {
        play_beep_pattern_tones(session.tx, kind);
        i2s_write_silence(session.tx, 128);
        audio_session_close(&session);
    }

    xSemaphoreGive(s_chime_mutex);
    return err;
}

#endif
