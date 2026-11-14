// png.c - writes 32-bit XRGB pixels as a PNG (zlib), for frame dumps (P3_FRAMES).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8];
    put32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = crc32(0, h + 4, 4);
    crc = crc32(crc, data, n);
    uint8_t t[4];
    put32(t, crc);
    fwrite(t, 1, 4, f);
}

// pixels: w*h 32-bit 0x00RRGGBB, row stride `pitch` bytes.
int png_write(const char *path, const uint8_t *pixels, int w, int h, int pitch) {
    size_t raw_n = (size_t)h * (1 + 3 * w);
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; ++y) {
        uint8_t *o = raw + (size_t)y * (1 + 3 * w);
        const uint32_t *row = (const uint32_t *)(pixels + (size_t)y * pitch);
        *o++ = 0;                                          // filter: none
        for (int x = 0; x < w; ++x) { uint32_t p = row[x]; *o++ = p >> 16; *o++ = p >> 8; *o++ = p; }
    }
    uLongf zn = compressBound(raw_n);
    uint8_t *z = malloc(zn);
    compress2(z, &zn, raw, raw_n, 6);
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, w); put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   // 8-bit RGB
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}
