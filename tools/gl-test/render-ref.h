// CPU oracle for the native offscreen render reference (128x128 RGBA8Unorm,
// red triangle on blue). Same constants and coverage rule as the graphics
// owner's metal-render-reference/render-ref.h and render-config.h; keep them
// equal. Triangle: 4,656 red pixels, FNV-1a64 16ad0cdb52fd60a5. Clear: solid
// blue, c99e7487b76ca325. Canary pixels are those with alpha 0x5a, which no
// expected pixel has.
#ifndef M3GPU_G15_RENDER_REF_H
#define M3GPU_G15_RENDER_REF_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define M3R_WIDTH 128
#define M3R_HEIGHT 128
#define M3R_X0 16
#define M3R_Y0 16
#define M3R_X1 113
#define M3R_Y1 16
#define M3R_X2 16
#define M3R_Y2 112
#define M3R_BYTES ((size_t)M3R_WIDTH * M3R_HEIGHT * 4)
#define M3R_CANARY_ALPHA 0x5a
#define M3R_TRIANGLE_FNV UINT64_C(0x16ad0cdb52fd60a5)
#define M3R_CLEAR_FNV UINT64_C(0xc99e7487b76ca325)
#define M3R_TRIANGLE_RED 4656

enum m3r_case { M3R_CLEAR, M3R_TRIANGLE };

static inline int m3r_parse_case(const char *name, enum m3r_case *c) {
    if (!strcmp(name, "triangle")) { *c = M3R_TRIANGLE; return 1; }
    if (!strcmp(name, "clear")) { *c = M3R_CLEAR; return 1; }
    return 0;
}

// Integer edge functions at pixel centres; no centre lies on an edge.
static inline int m3r_inside(int x, int y) {
    const int vx[] = {2 * M3R_X0, 2 * M3R_X1, 2 * M3R_X2};
    const int vy[] = {2 * M3R_Y0, 2 * M3R_Y1, 2 * M3R_Y2};
    const int px = 2 * x + 1, py = 2 * y + 1;
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        if ((vx[j] - vx[i]) * (py - vy[i]) - (vy[j] - vy[i]) * (px - vx[i]) <= 0)
            return 0;
    }
    return 1;
}

static inline void m3r_expected(enum m3r_case c, uint8_t *out) {
    for (int y = 0; y < M3R_HEIGHT; ++y)
        for (int x = 0; x < M3R_WIDTH; ++x) {
            uint8_t *p = out + ((size_t)y * M3R_WIDTH + x) * 4;
            int red = c == M3R_TRIANGLE && m3r_inside(x, y);
            p[0] = red ? 255 : 0;
            p[1] = 0;
            p[2] = red ? 0 : 255;
            p[3] = 255;
        }
}

static inline uint64_t m3r_hash(const uint8_t *bytes, size_t size) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}

struct m3r_check {
    uint32_t mismatched_pixels, red_pixels, canary_pixels;
    uint64_t actual_hash, expected_hash;
};

static inline struct m3r_check m3r_check_pixels(const uint8_t *actual, const uint8_t *expected) {
    struct m3r_check r = {0};
    for (size_t i = 0; i < M3R_BYTES; i += 4) {
        r.mismatched_pixels += memcmp(actual + i, expected + i, 4) != 0;
        r.red_pixels += actual[i] == 255 && actual[i + 1] == 0 && actual[i + 2] == 0 && actual[i + 3] == 255;
        r.canary_pixels += actual[i + 3] == M3R_CANARY_ALPHA;
    }
    r.actual_hash = m3r_hash(actual, M3R_BYTES);
    r.expected_hash = m3r_hash(expected, M3R_BYTES);
    return r;
}

#endif
