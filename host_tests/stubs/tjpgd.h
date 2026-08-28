// Host-test stub for esp_jpeg's vendored TJpgDec tjpgd.h — the streaming
// JPEG decode path (jd_prepare()/jd_decomp()) is not exercised on host, so
// the stub jd_prepare() always reports failure and jd_decomp() is never
// reached; PNG paths stay testable. Mirrors this repo's own
// jpg_stream_buffer_infunc()/jpg_stream_ring_infunc() signatures
// (`unsigned int`, not the real header's `size_t`) rather than the real
// vendored header directly: size_t is 32-bit on the actual ESP32 target but
// 64-bit on a host build, so copying it verbatim would silently mismatch
// the function-pointer types image_processor.c actually defines.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JDR_OK = 0,
    JDR_INTR,
    JDR_INP,
    JDR_MEM1,
    JDR_MEM2,
    JDR_PAR,
    JDR_FMT1,
    JDR_FMT2,
    JDR_FMT3
} JRESULT;

typedef struct {
    uint16_t left;
    uint16_t right;
    uint16_t top;
    uint16_t bottom;
} JRECT;

typedef struct JDEC JDEC;
struct JDEC {
    size_t dctr;
    uint8_t *dptr;
    uint8_t *inbuf;
    uint8_t dbit;
    uint8_t scale;
    uint8_t msx, msy;
    uint8_t qtid[3];
    uint8_t ncomp;
    int16_t dcv[3];
    uint16_t nrst;
    uint16_t width, height;
    uint8_t *huffbits[2][2];
    uint16_t *huffcode[2][2];
    uint8_t *huffdata[2][2];
    int32_t *qttbl[4];
    void *workbuf;
    uint8_t *mcubuf;
    void *pool;
    size_t sz_pool;
    unsigned int (*infunc)(JDEC *, uint8_t *, unsigned int);
    void *device;
};

JRESULT jd_prepare(JDEC *jd, unsigned int (*infunc)(JDEC *, uint8_t *, unsigned int), void *pool,
                    size_t sz_pool, void *dev);
JRESULT jd_decomp(JDEC *jd, int (*outfunc)(JDEC *, void *, JRECT *), uint8_t scale);

#ifdef __cplusplus
}
#endif
