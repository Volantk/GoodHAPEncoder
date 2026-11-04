#pragma once
#include <cstdint>

// Block encoders for Hap Q (scaled YCoCg in DXT5/BC3) and RGTC1/BC4 alpha.
//
// Everything works on float input (0..1, as stored in the source, no transfer
// change), so 10/16-bit sources keep their precision all the way into the
// endpoint and index fit. The fit minimises the error of the RGB the Hap Q
// shader reconstructs, not the error of the stored CoCg/Y values.

namespace ghap {

struct BlockSettings {
    int colorRadius = 1;       // +-N search around the fitted CoCg endpoints (5/6-bit)
    int alphaRadius = 4;       // +-N search around min/max for 8-bit BC4 endpoints
    int scaleTries = 4;        // chroma scales tried, from the largest that fits downwards
    float ditherChroma = 1.0f; // 0..1, in units of one third of a CoCg endpoint step
    float ditherLuma = 1.0f;   // 0..1, in units of one 8-bit level
    float ditherAlpha = 1.0f;  // 0..1, in units of one 8-bit level
    bool alphaWeighted = true; // weight colour error by alpha (Hap Q Alpha)
};

struct BlockInput {
    float r[16], g[16], b[16], a[16];
    // Dither noise per pixel in [-0.5, 0.5), one set per channel. May be null.
    const float* noiseCo = nullptr;
    const float* noiseCg = nullptr;
    const float* noiseY = nullptr;
    const float* noiseA = nullptr;
    bool hasAlpha = false;
};

// 16 bytes: BC4-style alpha block holding Y, then BC1 colour block holding Co, Cg, scale.
void encodeHapQBlock(const BlockInput& in, const BlockSettings& s, uint8_t out[16]);

// 8 bytes: RGTC1/BC4 block for 16 values in 0..1. weights may be null.
void encodeBC4Block(const float v[16], const float* weights, int radius, uint8_t out[8]);

} // namespace ghap
