#include "alarm_ramp.h"
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

esp_err_t board_hal_play_alarm(uint8_t volume_percent, uint32_t total_duration_ms,
                               bool (*should_stop)(void), const float notes_hz[4], uint32_t ramp_ms)
{
    (void) volume_percent;
    (void) total_duration_ms;
    (void) should_stop;
    (void) notes_hz;
    (void) ramp_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_play_notes(const board_hal_note_t *notes, int count, uint8_t volume_percent)
{
    (void) notes;
    (void) count;
    (void) volume_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

bool board_hal_has_microphone(void)
{
    return false;
}

esp_err_t board_hal_mic_capture(uint32_t duration_ms, board_hal_mic_block_cb_t on_block, void *user)
{
    (void) duration_ms;
    (void) on_block;
    (void) user;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_mic_capture_with_tones(uint32_t duration_ms, board_hal_mic_block_cb_t on_block,
                                           void *user, const board_hal_note_t *notes,
                                           int note_count, uint8_t volume_percent, uint32_t ramp_ms)
{
    (void) duration_ms;
    (void) on_block;
    (void) user;
    (void) notes;
    (void) note_count;
    (void) ramp_ms;
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
#include "esp_timer.h"
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

// The alarm may have to wait for a microphone test that it has just asked to stop.
#define ALARM_MUTEX_WAIT_MS 6000

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

#if BOARD_HAL_VOICE_ENABLED
// ---- ES7210 4-channel ADC (microphones) -----------------------------------
//
// The onboard microphones are not on the ES8311's own ADC input but on a
// separate ES7210 that shares the I2S bus (schematic: U5 ES7210, MIC1/MIC2
// capsules, MIC3 = speaker-amp reference for echo cancellation, SDOUT1 ->
// I2S_DSOUT = GPIO18). Register sequence follows Waveshare's own stock
// esp_codec_dev ES7210 driver as used by 01_Audio_Test ("in: {codec: ES7210}",
// MIC1 + MIC3 selected there; here MIC1 + MIC2 -> stereo slot 0 = MIC1, slot 1 = MIC2, slave mode,
// 16 kHz / 16 bit).
#define ES7210_I2C_ADDR_FIRST 0x40  // AD0/AD1 select 0x40..0x43
#define ES7210_I2C_ADDR_LAST 0x43
#define ES7210_GAIN_34_5DB \
    0x0C  // PGA gain register value: 3 dB steps, 0x0C = 34.5 dB (Waveshare's stock value)

static esp_err_t es7210_update_bits(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask,
                                    uint8_t value)
{
    uint8_t v = 0;
    esp_err_t err = es8311_read(dev, reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    return es8311_write(dev, reg, (uint8_t) ((v & (uint8_t) ~mask) | (value & mask)));
}

// Enables MIC1 and MIC2 (the two onboard capsules -> left/right I2S slot) -
// mirrors es7210_mic_select(). Waveshare's stock code selects MIC1 + MIC3 (MIC3
// = speaker-amp reference for echo cancellation), but the ES7210 routes ADC1/2
// to SDOUT1 (the only output wired to the ESP) and ADC3/4 to SDOUT2, so that
// reference never reached the right slot (measured: exactly silent).
static void es7210_select_mics(i2c_master_dev_handle_t dev, uint8_t gain)
{
    for (uint8_t i = 0; i < 4; i++) {
        es7210_update_bits(dev, (uint8_t) (0x43 + i), 0x10, 0x00);
    }
    es8311_write(dev, 0x4B, 0xFF);
    es8311_write(dev, 0x4C, 0xFF);
    // MIC1
    es7210_update_bits(dev, 0x01, 0x0B, 0x00);
    es8311_write(dev, 0x4B, 0x00);
    es7210_update_bits(dev, 0x43, 0x10, 0x10);
    es7210_update_bits(dev, 0x43, 0x0F, gain);
    // MIC2
    es7210_update_bits(dev, 0x01, 0x0B, 0x00);
    es8311_write(dev, 0x4B, 0x00);
    es7210_update_bits(dev, 0x44, 0x10, 0x10);
    es7210_update_bits(dev, 0x44, 0x0F, gain);
    es8311_write(dev, 0x12, 0x00);  // plain 2-channel I2S, no TDM
}

static esp_err_t es7210_mic_init(i2c_master_dev_handle_t dev, uint8_t gain)
{
    esp_err_t err = ESP_OK;
    // open()
    err |= es8311_write(dev, 0x00, 0xFF);
    err |= es8311_write(dev, 0x00, 0x41);
    err |= es8311_write(dev, 0x01, 0x3F);
    err |= es8311_write(dev, 0x09, 0x30);
    err |= es8311_write(dev, 0x0A, 0x30);
    err |= es8311_write(dev, 0x23, 0x2A);
    err |= es8311_write(dev, 0x22, 0x0A);
    err |= es8311_write(dev, 0x20, 0x0A);
    err |= es8311_write(dev, 0x21, 0x2A);
    err |= es7210_update_bits(dev, 0x08, 0x01, 0x00);  // I2S slave
    err |= es8311_write(dev, 0x40, 0x43);
    err |= es8311_write(dev, 0x41, 0x70);  // mic bias 2.87 V
    err |= es8311_write(dev, 0x42, 0x70);
    err |= es8311_write(dev, 0x07, 0x20);
    err |= es8311_write(dev, 0x02, 0xC1);
    es7210_select_mics(dev, 0);
    uint8_t off_reg = 0;
    es8311_read(dev, 0x01, &off_reg);
    // set_fs(): 16 bit, I2S format (slave mode: the sample-rate dividers stay unused)
    err |= es7210_update_bits(dev, 0x11, 0xE0, 0x60);
    err |= es7210_update_bits(dev, 0x11, 0x03, 0x00);
    // enable()/start()
    err |= es8311_write(dev, 0x01, off_reg);
    err |= es8311_write(dev, 0x06, 0x00);
    err |= es8311_write(dev, 0x40, 0x43);
    err |= es8311_write(dev, 0x47, 0x08);
    err |= es8311_write(dev, 0x48, 0x08);
    err |= es8311_write(dev, 0x49, 0x08);
    err |= es8311_write(dev, 0x4A, 0x08);
    es7210_select_mics(dev, 0);
    err |= es8311_write(dev, 0x40, 0x43);
    err |= es8311_write(dev, 0x00, 0x71);
    err |= es8311_write(dev, 0x00, 0x41);
    // Final PGA gain (the stock code applies it after enabling).
    err |= es7210_update_bits(dev, 0x43, 0x0F, gain);
    err |= es7210_update_bits(dev, 0x44, 0x0F, gain);
    return err;
}

static void es7210_standby(i2c_master_dev_handle_t dev)
{
    es8311_write(dev, 0x47, 0xFF);
    es8311_write(dev, 0x48, 0xFF);
    es8311_write(dev, 0x49, 0xFF);
    es8311_write(dev, 0x4A, 0xFF);
    es8311_write(dev, 0x4B, 0xFF);
    es8311_write(dev, 0x4C, 0xFF);
    es8311_write(dev, 0x40, 0xC0);
    es8311_write(dev, 0x01, 0x7F);
    es8311_write(dev, 0x06, 0x07);
}

#endif  // BOARD_HAL_VOICE_ENABLED

typedef struct {
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;  // only when opened with with_rx (microphone capture)
    i2c_master_dev_handle_t es8311;
    i2c_master_dev_handle_t es7210;  // microphone ADC, only with with_rx
} audio_session_t;

static void audio_session_close(audio_session_t *s)
{
    pa_set(false);
#if BOARD_HAL_VOICE_ENABLED
    if (s->es7210) {
        es7210_standby(s->es7210);
        i2c_master_bus_rm_device(s->es7210);
        s->es7210 = NULL;
    }
#endif
    if (s->es8311) {
        es8311_standby(s->es8311);
    }
    if (s->rx) {
        i2s_channel_disable(s->rx);
        i2s_del_channel(s->rx);
        s->rx = NULL;
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

// with_rx: also open the I2S receive path (ES7210 microphone ADC -> DIN) for
// microphone capture. enable_pa: switch the speaker amplifier on (playback); mic
// capture leaves it off so nothing is audible.
static esp_err_t audio_session_open(audio_session_t *s, uint8_t volume_percent, bool with_rx,
                                    bool enable_pa)
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
    err = i2s_new_channel(&chan_cfg, &s->tx, with_rx ? &s->rx : NULL);
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
                .din = with_rx ? BOARD_HAL_AUDIO_I2S_DIN_PIN : I2S_GPIO_UNUSED,
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
    if (err == ESP_OK && with_rx) {
        err = i2s_channel_init_std_mode(s->rx, &std_cfg);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s->tx);
    }
    if (err == ESP_OK && with_rx) {
        err = i2s_channel_enable(s->rx);
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

#if BOARD_HAL_VOICE_ENABLED
    if (with_rx) {
        // The microphones sit on the ES7210, which drives I2S DIN. Tri-state the
        // ES8311's own ADC output (SDP_OUT bit 6) so the two don't fight over it.
        uint8_t sdp_out = 0;
        if (es8311_read(s->es8311, ES8311_REG_SDP_OUT, &sdp_out) == ESP_OK) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                es8311_write(s->es8311, ES8311_REG_SDP_OUT, (uint8_t) (sdp_out | 0x40)));
        }

        i2c_master_bus_handle_t mic_bus = board_hal_get_i2c_bus();
        uint8_t mic_addr = 0;
        for (uint8_t a = ES7210_I2C_ADDR_FIRST; a <= ES7210_I2C_ADDR_LAST; a++) {
            if (i2c_master_probe(mic_bus, a, 50) == ESP_OK) {
                mic_addr = a;
                break;
            }
        }
        if (mic_addr == 0) {
            ESP_LOGE(TAG, "ES7210 microphone ADC not found on I2C 0x%02x-0x%02x",
                     ES7210_I2C_ADDR_FIRST, ES7210_I2C_ADDR_LAST);
            audio_session_close(s);
            return ESP_ERR_NOT_FOUND;
        }
        i2c_device_config_t mic_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = mic_addr,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(mic_bus, &mic_cfg, &s->es7210);
        if (err == ESP_OK) {
            err = es7210_mic_init(s->es7210, ES7210_GAIN_34_5DB);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ES7210 init failed: %s", esp_err_to_name(err));
            audio_session_close(s);
            return err;
        }
        ESP_LOGI(TAG, "ES7210 microphone ADC at I2C 0x%02x initialised", mic_addr);
    }

#endif  // BOARD_HAL_VOICE_ENABLED

    i2s_write_silence(s->tx, 128);
    if (!enable_pa) {
        vTaskDelay(pdMS_TO_TICKS(100));  // let the ADC front end settle
        return ESP_OK;
    }
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
    esp_err_t err = audio_session_open(&session, volume_percent, false, true);
    if (err == ESP_OK) {
        play_beep_pattern_tones(session.tx, kind);
        i2s_write_silence(session.tx, 128);
        audio_session_close(&session);
    }

    xSemaphoreGive(s_chime_mutex);
    return err;
}

// Alarm-clock tone sequence (docs/ALARMCLOCK_FEASIBILITY.md): G4-C5-E5-C5,
// 300ms each, then a 5s pause, repeating until total_duration_ms elapses or
// should_stop() reports true. The pause is written in small chunks (not one
// 5s i2s_write_silence() call) purely so should_stop() gets checked several
// times per pause instead of only once every 5 seconds.
esp_err_t board_hal_play_alarm(uint8_t volume_percent, uint32_t total_duration_ms,
                               bool (*should_stop)(void), const float notes_hz[4], uint32_t ramp_ms)
{
    chime_mutex_init();
    if (!s_chime_mutex ||
        xSemaphoreTake(s_chime_mutex, pdMS_TO_TICKS(ALARM_MUTEX_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    static const float DEFAULT_NOTES_HZ[4] = {392.0f, 523.0f, 659.0f, 523.0f};  // G4 C5 E5 C5
    const float *alarm_notes = notes_hz ? notes_hz : DEFAULT_NOTES_HZ;
    const int NOTE_MS = 300;
    const int PAUSE_MS = 5000;
    const int PAUSE_CHUNK_MS = 200;

    audio_session_t session;
    esp_err_t err = audio_session_open(&session, volume_percent, false, true);
    if (err == ESP_OK) {
        uint32_t elapsed_ms = 0;
        bool stop = false;
        while (elapsed_ms < total_duration_ms && !stop) {
            for (int i = 0; i < 4 && !stop; i++) {
                // Volume ramp-up: the gain follows the time since the ring started.
                float gain = alarm_ramp_gain(elapsed_ms + NOTE_MS / 2, ramp_ms);
                play_tone(session.tx, alarm_notes[i], NOTE_MS,
                          (int) ((float) CHIME_AMPLITUDE * gain));
                elapsed_ms += NOTE_MS;
                if (should_stop && should_stop()) {
                    stop = true;
                }
            }
            int pause_remaining_ms = PAUSE_MS;
            while (pause_remaining_ms > 0 && !stop) {
                int chunk_ms =
                    pause_remaining_ms < PAUSE_CHUNK_MS ? pause_remaining_ms : PAUSE_CHUNK_MS;
                i2s_write_silence(session.tx, CHIME_SAMPLE_RATE * chunk_ms / 1000);
                pause_remaining_ms -= chunk_ms;
                elapsed_ms += (uint32_t) chunk_ms;
                if (should_stop && should_stop()) {
                    stop = true;
                }
            }
        }
        i2s_write_silence(session.tx, 128);
        audio_session_close(&session);
    }

    xSemaphoreGive(s_chime_mutex);
    return err;
}

// General-purpose note-sequence player behind the alarm-setting button UI's
// feedback sounds - see board_hal.h's own doc comment on why this plays the
// whole sequence inside one session rather than one open/close per note.
esp_err_t board_hal_play_notes(const board_hal_note_t *notes, int count, uint8_t volume_percent)
{
    chime_mutex_init();
    if (!s_chime_mutex || xSemaphoreTake(s_chime_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    audio_session_t session;
    esp_err_t err = audio_session_open(&session, volume_percent, false, true);
    if (err == ESP_OK) {
        for (int i = 0; i < count; i++) {
            if (notes[i].freq_hz > 0.0f) {
                play_tone(session.tx, notes[i].freq_hz, notes[i].duration_ms, CHIME_AMPLITUDE);
            } else {
                i2s_write_silence(session.tx, CHIME_SAMPLE_RATE * notes[i].duration_ms / 1000);
            }
        }
        i2s_write_silence(session.tx, 128);
        audio_session_close(&session);
    }

    xSemaphoreGive(s_chime_mutex);
    return err;
}

#if BOARD_HAL_VOICE_ENABLED

bool board_hal_has_microphone(void)
{
    return true;
}

// Steps through a note sequence one block at a time (a tone or silence per
// note, 8 ms attack/release like play_tone()), then stays silent.
typedef struct {
    const board_hal_note_t *notes;
    int count;
    int idx;
    int frame;
    float phase;
    uint32_t ramp_ms;        // volume ramp-up (0 = none), see alarm_ramp.h
    uint32_t frames_played;  // frames produced so far
} tone_seq_t;

static void tone_seq_fill(tone_seq_t *t, int16_t *out, int frames)
{
    const int edge = CHIME_SAMPLE_RATE * 8 / 1000;
    const float gain = alarm_ramp_gain(
        (uint32_t) ((uint64_t) t->frames_played * 1000u / CHIME_SAMPLE_RATE), t->ramp_ms);
    t->frames_played += (uint32_t) frames;
    for (int i = 0; i < frames; i++) {
        int16_t sample = 0;
        if (t->idx < t->count) {
            const board_hal_note_t *note = &t->notes[t->idx];
            int n = CHIME_SAMPLE_RATE * note->duration_ms / 1000;
            if (note->freq_hz > 0.0f && n > 0) {
                float env = 1.0f;
                if (t->frame < edge) {
                    env = (float) t->frame / (float) edge;
                } else if (t->frame > n - edge) {
                    env = (float) (n - t->frame) / (float) edge;
                }
                sample = (int16_t) (sinf(t->phase) * (float) CHIME_AMPLITUDE * env * gain);
                t->phase += 2.0f * (float) M_PI * note->freq_hz / (float) CHIME_SAMPLE_RATE;
                if (t->phase > 2.0f * (float) M_PI) {
                    t->phase -= 2.0f * (float) M_PI;
                }
            }
            if (++t->frame >= n) {
                t->idx++;
                t->frame = 0;
                t->phase = 0.0f;
            }
        }
        out[i * 2] = sample;
        out[i * 2 + 1] = sample;
    }
}

// Shared by board_hal_mic_capture() and board_hal_mic_capture_with_tones():
// with a note sequence the speaker plays it while the microphone is read, in
// the same full-duplex I2S session.
static esp_err_t mic_capture_impl(uint32_t duration_ms, board_hal_mic_block_cb_t on_block,
                                  void *user, const board_hal_note_t *notes, int note_count,
                                  uint8_t volume_percent, uint32_t ramp_ms)
{
    if (!on_block) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool play = notes != NULL && note_count > 0;
    chime_mutex_init();
    if (!s_chime_mutex ||
        xSemaphoreTake(s_chime_mutex, pdMS_TO_TICKS(play ? ALARM_MUTEX_WAIT_MS : 2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    audio_session_t session;
    esp_err_t err = audio_session_open(&session, play ? volume_percent : 0, true, play);
    if (err == ESP_OK) {
        // 256 stereo frames = 16 ms per block at 16 kHz. The first blocks after
        // start-up are discarded: the analog front end of the codec still settles.
        const int SETTLE_BLOCKS = BOARD_HAL_MIC_SETTLE_FRAMES / 256;
        int16_t buf[256 * 2];
        int16_t tx[256 * 2];
        tone_seq_t seq = {.notes = notes, .count = play ? note_count : 0, .ramp_ms = ramp_ms};
        int64_t end_us = esp_timer_get_time() + (int64_t) duration_ms * 1000;
        int block = 0;
        bool keep_going = true;
        while (keep_going && esp_timer_get_time() < end_us) {
            if (play) {
                tone_seq_fill(&seq, tx, 256);
                size_t written = 0;
                i2s_channel_write(session.tx, tx, sizeof(tx), &written, pdMS_TO_TICKS(200));
            }
            size_t bytes = 0;
            err = i2s_channel_read(session.rx, buf, sizeof(buf), &bytes, pdMS_TO_TICKS(200));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
                break;
            }
            if (++block > SETTLE_BLOCKS) {
                keep_going = on_block(buf, bytes / 4, user);
            }
        }
        if (play) {
            i2s_write_silence(session.tx, 128);
        }
        audio_session_close(&session);
    }

    xSemaphoreGive(s_chime_mutex);
    return err;
}

esp_err_t board_hal_mic_capture(uint32_t duration_ms, board_hal_mic_block_cb_t on_block, void *user)
{
    return mic_capture_impl(duration_ms, on_block, user, NULL, 0, 0, 0);
}

esp_err_t board_hal_mic_capture_with_tones(uint32_t duration_ms, board_hal_mic_block_cb_t on_block,
                                           void *user, const board_hal_note_t *notes,
                                           int note_count, uint8_t volume_percent, uint32_t ramp_ms)
{
    return mic_capture_impl(duration_ms, on_block, user, notes, note_count, volume_percent,
                            ramp_ms);
}

#else  // !BOARD_HAL_VOICE_ENABLED

bool board_hal_has_microphone(void)
{
    return false;
}

esp_err_t board_hal_mic_capture(uint32_t duration_ms, board_hal_mic_block_cb_t on_block, void *user)
{
    (void) duration_ms;
    (void) on_block;
    (void) user;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_mic_capture_with_tones(uint32_t duration_ms, board_hal_mic_block_cb_t on_block,
                                           void *user, const board_hal_note_t *notes,
                                           int note_count, uint8_t volume_percent, uint32_t ramp_ms)
{
    (void) duration_ms;
    (void) on_block;
    (void) user;
    (void) notes;
    (void) note_count;
    (void) ramp_ms;
    (void) volume_percent;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif  // BOARD_HAL_VOICE_ENABLED

#endif
