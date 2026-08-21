// Standalone host copy of image_processor.c's sanitize_caption_ascii(),
// exposed as image_processor_sanitize_ascii() - headlines.c's only
// dependency on image_processor.h. Kept as an independent copy (rather than
// linking the real image_processor.c) since that would drag in libpng, the
// JPEG decoder, and board_hal for no benefit to this test - see
// image_pipeline_test/display_flow_test for tests that DO need the real
// pipeline. MUST be kept behaviorally identical to the real function.
#include <stddef.h>
#include <stdint.h>

void image_processor_sanitize_ascii(const char *utf8, char *out, size_t out_len)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *) utf8;

    while (*p != '\0' && o + 1 < out_len) {
        unsigned char b0 = p[0];

        if (b0 < 0x80) {
            out[o++] = (char) b0;
            p++;
            continue;
        }

        uint32_t cp = 0;
        int extra;
        if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            extra = 3;
        } else {
            p++;  // stray continuation/invalid byte, skip
            continue;
        }

        int valid = 1;
        for (int i = 0; i < extra; i++) {
            unsigned char cb = p[1 + i];
            if ((cb & 0xC0) != 0x80) {
                valid = 0;
                break;
            }
            cp = (cp << 6) | (cb & 0x3F);
        }
        if (!valid) {
            p++;
            continue;
        }
        p += 1 + extra;

        const char *sub = NULL;
        switch (cp) {
        case 0x00E4:
            sub = "ae";
            break;  // ä
        case 0x00F6:
            sub = "oe";
            break;  // ö
        case 0x00FC:
            sub = "ue";
            break;  // ü
        case 0x00C4:
            sub = "Ae";
            break;  // Ä
        case 0x00D6:
            sub = "Oe";
            break;  // Ö
        case 0x00DC:
            sub = "Ue";
            break;  // Ü
        case 0x00DF:
            sub = "ss";
            break;  // ß
        default:
            break;  // everything else (accents, emoji, symbols, ...) dropped
        }
        for (const char *s = sub; s && *s != '\0' && o + 1 < out_len; s++) {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
}
