#ifndef HTTPS_CERT_H
#define HTTPS_CERT_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// On-device self-signed HTTPS certificate (github.com/aitjcize/esp32-photoframe#130).
// A per-device ECDSA P-256 keypair + self-signed cert is generated once on
// first use and persisted (NVS blobs), rather than a single key baked into
// every device's firmware image - a shared key shipped in a public repo
// would let anyone impersonate/MITM any device running this firmware,
// defeating the point of using HTTPS at all. Browsers will still show a
// self-signed-certificate warning (expected, no CA can vouch for a device
// with no public hostname) - this protects the password/session from
// passive LAN sniffing, not from an active attacker who ignores that
// warning.
//
// Returns cached/stored DER-encoded cert and private key bytes via
// `*out_cert`/`*out_key` (owned by this module, valid for the life of the
// program - do not free), generating and persisting them first if this is
// the very first call. ESP_FAIL if generation fails (PSA/mbedtls error) or
// storage fails - callers should fall back to plain HTTP in that case
// rather than fail to serve the web UI at all.
esp_err_t https_cert_get(const uint8_t **out_cert_der, size_t *out_cert_len,
                         const uint8_t **out_key_der, size_t *out_key_len);

#endif
