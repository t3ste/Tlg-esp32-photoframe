#include "https_cert.h"

#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

static const char *TAG = "https_cert";

// 14/13 chars - within NVS's 15-char key limit.
#define NVS_HTTPS_CERT_KEY "https_cert_der"
#define NVS_HTTPS_KEY_KEY "https_key_der"

#define CERT_DER_MAX_LEN 512
#define KEY_DER_MAX_LEN 256

static uint8_t *s_cert_der = NULL;
static size_t s_cert_len = 0;
static uint8_t *s_key_der = NULL;
static size_t s_key_len = 0;

static bool load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t cert_len = CERT_DER_MAX_LEN;
    uint8_t *cert_buf = malloc(CERT_DER_MAX_LEN);
    size_t key_len = KEY_DER_MAX_LEN;
    uint8_t *key_buf = malloc(KEY_DER_MAX_LEN);
    bool ok = false;

    if (cert_buf && key_buf && nvs_get_blob(h, NVS_HTTPS_CERT_KEY, cert_buf, &cert_len) == ESP_OK &&
        nvs_get_blob(h, NVS_HTTPS_KEY_KEY, key_buf, &key_len) == ESP_OK && cert_len > 0 &&
        key_len > 0) {
        s_cert_der = cert_buf;
        s_cert_len = cert_len;
        s_key_der = key_buf;
        s_key_len = key_len;
        ok = true;
    } else {
        free(cert_buf);
        free(key_buf);
    }

    nvs_close(h);
    return ok;
}

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_blob(h, NVS_HTTPS_CERT_KEY, s_cert_der, s_cert_len);
    nvs_set_blob(h, NVS_HTTPS_KEY_KEY, s_key_der, s_key_len);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Generates a per-device ECDSA P-256 keypair (PSA Crypto - the only key
// generation API left in this mbedtls version; the classic
// mbedtls_pk_setup()+mbedtls_ecp_gen_key() sequence from older mbedtls
// tutorials was removed entirely in the TF-PSA-Crypto rewrite this project's
// mbedtls version ships - confirmed against the actual installed headers,
// see tf-psa-crypto/docs/4.0-migration-guide/pk.md) and a self-signed
// X.509 certificate wrapping its public key, both DER-encoded into the
// module-level buffers above ready for NVS storage.
static esp_err_t generate_self_signed(void)
{
    psa_status_t pstat = psa_crypto_init();
    if (pstat != PSA_SUCCESS && pstat != PSA_ERROR_ALREADY_EXISTS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int) pstat);
        return ESP_FAIL;
    }

    psa_key_attributes_t attrs = psa_key_attributes_init();
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attrs, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id;
    pstat = psa_generate_key(&attrs, &key_id);
    if (pstat != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key failed: %d", (int) pstat);
        return ESP_FAIL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_wrap_psa(&pk, key_id);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_wrap_psa failed: -0x%04x", -ret);
        psa_destroy_key(key_id);
        return ESP_FAIL;
    }

    uint8_t *key_buf = malloc(KEY_DER_MAX_LEN);
    if (!key_buf) {
        mbedtls_pk_free(&pk);
        psa_destroy_key(key_id);
        return ESP_ERR_NO_MEM;
    }
    // mbedtls_pk_write_key_der() writes backwards from the end of the
    // buffer, returning the actual (smaller) length - matches every other
    // mbedtls DER-writer used below.
    int key_written = mbedtls_pk_write_key_der(&pk, key_buf, KEY_DER_MAX_LEN);
    if (key_written < 0) {
        ESP_LOGE(TAG, "mbedtls_pk_write_key_der failed: -0x%04x", -key_written);
        free(key_buf);
        mbedtls_pk_free(&pk);
        psa_destroy_key(key_id);
        return ESP_FAIL;
    }

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);  // self-signed

    // "photoframe" (the device's own mDNS name defaults to this too, per
    // mdns_service.c) - a fixed, generic CN rather than a per-device one
    // deliberately: this cert is never meant to be pinned/verified by a
    // client against a specific hostname, only to encrypt the link, so
    // there's nothing to gain from making it more specific and one more
    // thing to regenerate if the device's own name changes later.
    ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=photoframe,O=esp32-photoframe");
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=photoframe,O=esp32-photoframe");
    }
    // Wide, clock-independent validity window - this device's own wall
    // clock may not be correct yet at first boot (before the first SNTP
    // sync), and a self-signed cert's dates are not meaningfully verified
    // by anyone here anyway (see this file's own top-of-file comment).
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20440101000000");
    }
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    }
    // Random serial, not sequential/predictable - required to be non-empty
    // by the DER encoding either way.
    uint8_t serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7F;  // keep it a positive INTEGER in DER
    if (ret == 0) {
        ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    }

    uint8_t *cert_buf = NULL;
    int cert_written = -1;
    if (ret == 0) {
        cert_buf = malloc(CERT_DER_MAX_LEN);
        if (!cert_buf) {
            ret = -1;
        } else {
            cert_written = mbedtls_x509write_crt_der(&crt, cert_buf, CERT_DER_MAX_LEN);
            if (cert_written < 0) {
                ret = cert_written;
            }
        }
    }

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);  // does not destroy the wrapped PSA key - see its own doc comment
    psa_destroy_key(key_id);

    if (ret != 0 || cert_written < 0) {
        ESP_LOGE(TAG, "Certificate generation failed: -0x%04x", -ret);
        free(key_buf);
        free(cert_buf);
        return ESP_FAIL;
    }

    // Both *_der writers fill the buffer end-aligned - the real content is
    // the last `written` bytes, not the first (matches mbedtls's own
    // documented convention for every DER-writing function used above).
    s_key_len = (size_t) key_written;
    s_key_der = malloc(s_key_len);
    if (s_key_der) {
        memcpy(s_key_der, key_buf + KEY_DER_MAX_LEN - key_written, s_key_len);
    }
    s_cert_len = (size_t) cert_written;
    s_cert_der = malloc(s_cert_len);
    if (s_cert_der) {
        memcpy(s_cert_der, cert_buf + CERT_DER_MAX_LEN - cert_written, s_cert_len);
    }
    free(key_buf);
    free(cert_buf);

    if (!s_key_der || !s_cert_der) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Generated new self-signed HTTPS certificate (%zu bytes) and key (%zu bytes)",
             s_cert_len, s_key_len);
    return ESP_OK;
}

esp_err_t https_cert_get(const uint8_t **out_cert_der, size_t *out_cert_len,
                         const uint8_t **out_key_der, size_t *out_key_len)
{
    if (!s_cert_der || !s_key_der) {
        if (!load_from_nvs()) {
            esp_err_t err = generate_self_signed();
            if (err != ESP_OK) {
                return err;
            }
            esp_err_t save_err = save_to_nvs();
            if (save_err != ESP_OK) {
                // Not fatal - this boot can still use the freshly-generated
                // cert from RAM, it just won't survive a reboot, which will
                // regenerate a fresh one again. Worth logging loudly since
                // a device that never persists this would generate (and
                // pointlessly burn PSA/heap on) a new cert every boot.
                ESP_LOGW(TAG,
                         "Failed to persist HTTPS certificate (%s) - will regenerate next boot",
                         esp_err_to_name(save_err));
            }
        }
    }

    *out_cert_der = s_cert_der;
    *out_cert_len = s_cert_len;
    *out_key_der = s_key_der;
    *out_key_len = s_key_len;
    return ESP_OK;
}
