#include "exif_reader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "exif_reader";

#define EXIF_TAG_EXIF_IFD_POINTER 0x8769
#define EXIF_TAG_DATETIME_ORIGINAL 0x9003
#define EXIF_TYPE_ASCII 2

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint16_t read_u16(const uint8_t *p, bool big_endian)
{
    return big_endian ? (uint16_t) ((p[0] << 8) | p[1]) : (uint16_t) ((p[1] << 8) | p[0]);
}

static uint32_t read_u32(const uint8_t *p, bool big_endian)
{
    return big_endian
               ? ((uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3])
               : ((uint32_t) p[3] << 24 | (uint32_t) p[2] << 16 | (uint32_t) p[1] << 8 | p[0]);
}

// Searches one IFD (Image File Directory) at `ifd_offset` (relative to the
// start of the TIFF structure, i.e. `buf`) for `tag`. On a match, fills
// *out_type/*out_count and *out_value_field_pos (the byte offset within
// `buf` of the entry's 4-byte value/offset field - ASCII values <= 4 bytes
// would be stored inline there, longer ones are an offset from `buf`;
// DateTimeOriginal is always 20 bytes, so this code only ever treats it as
// an offset).
static bool find_ifd_tag(const uint8_t *buf, size_t buf_len, uint32_t ifd_offset, bool big_endian,
                         uint16_t tag, uint16_t *out_type, uint32_t *out_count,
                         uint32_t *out_value_field_pos)
{
    if ((size_t) ifd_offset + 2 > buf_len) {
        return false;
    }
    uint16_t entry_count = read_u16(buf + ifd_offset, big_endian);
    for (uint16_t i = 0; i < entry_count; i++) {
        uint32_t entry_pos = ifd_offset + 2 + (uint32_t) i * 12;
        if ((size_t) entry_pos + 12 > buf_len) {
            break;
        }
        uint16_t entry_tag = read_u16(buf + entry_pos, big_endian);
        if (entry_tag == tag) {
            *out_type = read_u16(buf + entry_pos + 2, big_endian);
            *out_count = read_u32(buf + entry_pos + 4, big_endian);
            *out_value_field_pos = entry_pos + 8;
            return true;
        }
    }
    return false;
}

// Scans the JPEG's marker segments for the Exif APP1 segment, reads its TIFF
// payload into a heap buffer (bounded by the marker's own 16-bit length, max
// 65533 bytes) and returns it. Caller frees. Returns NULL if not found/not a
// JPEG.
static uint8_t *read_exif_tiff_payload(const char *path, size_t *out_len)
{
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }

    uint8_t soi[2];
    if (fread(soi, 1, 2, f) != 2 || soi[0] != 0xFF || soi[1] != 0xD8) {
        fclose(f);
        return NULL;  // not a JPEG
    }

    uint8_t *tiff_buf = NULL;
    while (true) {
        uint8_t m[2];
        if (fread(m, 1, 2, f) != 2 || m[0] != 0xFF) {
            break;
        }
        uint8_t marker = m[1];
        // Markers with no length field (padding, RST0-7, SOI/EOI) carry no
        // payload to skip.
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
            if (marker == 0xD9) {
                break;  // EOI
            }
            continue;
        }
        if (marker == 0xDA) {
            break;  // Start of scan - all metadata segments precede this
        }

        uint8_t len_be[2];
        if (fread(len_be, 1, 2, f) != 2) {
            break;
        }
        uint16_t seg_len = read_be16(len_be);
        if (seg_len < 2) {
            break;
        }
        uint16_t data_len = seg_len - 2;
        long data_start = ftell(f);

        if (marker == 0xE1 && data_len > 8) {
            uint8_t header[6];
            if (fread(header, 1, 6, f) == 6 && memcmp(header, "Exif\0\0", 6) == 0) {
                size_t tiff_len = (size_t) data_len - 6;
                tiff_buf = malloc(tiff_len);
                if (tiff_buf && fread(tiff_buf, 1, tiff_len, f) == tiff_len) {
                    *out_len = tiff_len;
                    fclose(f);
                    return tiff_buf;
                }
                free(tiff_buf);
                tiff_buf = NULL;
            }
        }

        if (fseek(f, data_start + data_len, SEEK_SET) != 0) {
            break;
        }
    }

    fclose(f);
    return NULL;
}

bool exif_reader_get_datetime_original(const char *path, char *out, size_t out_len)
{
    if (!path || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    size_t tiff_len = 0;
    uint8_t *tiff = read_exif_tiff_payload(path, &tiff_len);
    if (!tiff || tiff_len < 8) {
        free(tiff);
        return false;
    }

    bool big_endian = (tiff[0] == 'M' && tiff[1] == 'M');
    bool little_endian = (tiff[0] == 'I' && tiff[1] == 'I');
    if (!big_endian && !little_endian) {
        ESP_LOGW(TAG, "%s: Exif segment has no valid TIFF byte-order marker", path);
        free(tiff);
        return false;
    }

    uint32_t ifd0_offset = read_u32(tiff + 4, big_endian);

    uint16_t exif_ptr_type;
    uint32_t exif_ptr_count, exif_ptr_field_pos;
    bool ok = false;

    if (find_ifd_tag(tiff, tiff_len, ifd0_offset, big_endian, EXIF_TAG_EXIF_IFD_POINTER,
                     &exif_ptr_type, &exif_ptr_count, &exif_ptr_field_pos) &&
        (size_t) exif_ptr_field_pos + 4 <= tiff_len) {
        uint32_t exif_ifd_offset = read_u32(tiff + exif_ptr_field_pos, big_endian);

        uint16_t type;
        uint32_t count, value_field_pos;
        if (find_ifd_tag(tiff, tiff_len, exif_ifd_offset, big_endian, EXIF_TAG_DATETIME_ORIGINAL,
                         &type, &count, &value_field_pos) &&
            type == EXIF_TYPE_ASCII && count >= 19 && (size_t) value_field_pos + 4 <= tiff_len) {
            uint32_t str_offset = read_u32(tiff + value_field_pos, big_endian);
            if ((size_t) str_offset + 19 <= tiff_len) {
                const char *s = (const char *) (tiff + str_offset);
                // "YYYY:MM:DD HH:MM:SS" -> "YYYY-MM-DD HH:MM" (seconds
                // dropped - not meaningful for a caption line).
                if (s[4] == ':' && s[7] == ':' && s[10] == ' ' && s[13] == ':') {
                    snprintf(out, out_len, "%.4s-%.2s-%.2s %.2s:%.2s", s, s + 5, s + 8, s + 11,
                             s + 14);
                    ok = true;
                }
            }
        }
    }

    free(tiff);
    return ok;
}

// A hand-sized JSON sidecar (see process-cli/capture-date.js) is well under
// a few KB - reject anything wildly out of shape rather than allocating an
// unbounded buffer for a malformed or hostile file.
#define CAPTURE_DATE_JSON_MAX_SIZE 4096

// Derives "<dir>/<name>.capture.json" from a displayed image's anchor path,
// stripping any ".fit" suffix, or - for a ".cover" variant living in the
// "crop/" subdirectory per that feature's own convention (see
// docs/FACE_CROP.md) - both the ".cover" suffix and the "crop/" directory
// component, so all three possible anchor shapes for the same source photo
// (bare "<name>.<ext>", "<name>.fit.<ext>", "crop/<name>.cover.<ext>")
// resolve to the identical sidecar path process-cli itself always writes to:
// captureDatePathFor() (process-cli/capture-date.js) derives from the output
// base path before any variant suffix is appended, once per source image,
// never per rendered variant. Mirrors facecrop_metadata.c's
// derive_metadata_path() for the analogous ".facecrop.json" sidecar.
static bool derive_capture_date_path(const char *anchor_path, char *out, size_t out_size)
{
    char dir[512] = "";
    const char *slash = strrchr(anchor_path, '/');
    const char *filename = anchor_path;
    if (slash) {
        size_t dir_len = (size_t) (slash - anchor_path);
        if (dir_len >= sizeof(dir)) {
            return false;
        }
        memcpy(dir, anchor_path, dir_len);
        dir[dir_len] = '\0';
        filename = slash + 1;
    }

    const char *dot = strrchr(filename, '.');
    if (!dot) {
        return false;
    }
    size_t name_len = (size_t) (dot - filename);

    bool is_cover = (name_len > 6 && strncasecmp(filename + name_len - 6, ".cover", 6) == 0);
    bool is_fit =
        !is_cover && (name_len > 4 && strncasecmp(filename + name_len - 4, ".fit", 4) == 0);
    if (is_cover) {
        name_len -= 6;
        // ".cover" files live one level down, in "crop/" - go back up to the
        // directory the original source photo (and its sidecars) live in.
        static const char CROP_DIRNAME[] = "crop";
        size_t dir_len = strlen(dir);
        if (dir_len >= sizeof(CROP_DIRNAME) - 1 &&
            strcasecmp(dir + dir_len - (sizeof(CROP_DIRNAME) - 1), CROP_DIRNAME) == 0) {
            char *crop_slash = strrchr(dir, '/');
            if (crop_slash) {
                *crop_slash = '\0';
            } else {
                dir[0] = '\0';
            }
        }
    } else if (is_fit) {
        name_len -= 4;
    }
    if (name_len == 0 || name_len >= 128) {
        return false;
    }

    char base[128];
    memcpy(base, filename, name_len);
    base[name_len] = '\0';

    int written = snprintf(out, out_size, "%s%s%s.capture.json", dir, dir[0] ? "/" : "", base);
    return written > 0 && (size_t) written < out_size;
}

bool capture_date_sidecar_read(const char *anchor_path, char *out, size_t out_len)
{
    if (!anchor_path || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    char sidecar_path[320];
    if (!derive_capture_date_path(anchor_path, sidecar_path, sizeof(sidecar_path))) {
        return false;
    }

    FILE *fp = fopen(sidecar_path, "rb");
    if (!fp) {
        return false;  // No sidecar - not an error, just "unavailable"
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > CAPTURE_DATE_JSON_MAX_SIZE) {
        fclose(fp);
        return false;
    }

    char *buffer = malloc((size_t) size + 1);
    if (!buffer) {
        fclose(fp);
        return false;
    }
    size_t read_bytes = fread(buffer, 1, (size_t) size, fp);
    fclose(fp);
    buffer[read_bytes] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse %s", sidecar_path);
        return false;
    }

    bool ok = false;
    cJSON *capture_date = cJSON_GetObjectItem(root, "capture_date");
    if (cJSON_IsString(capture_date) && capture_date->valuestring[0] != '\0') {
        strncpy(out, capture_date->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
        ok = true;
    } else {
        ESP_LOGW(TAG, "%s has no valid capture_date", sidecar_path);
    }

    cJSON_Delete(root);
    return ok;
}
