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
    // Size/quality trade for snappy: extra squared RGB error (0..1 units, summed
    // over the block) accepted per byte saved. 0 = best quality, no reuse.
    float rdoLambda = 0.0f;
    int rdoWindow = 32; // earlier blocks in the same row to reuse bytes from
    // Activity masking: a block's lambda is scaled by (its RGB standard deviation /
    // rdoMaskRef)^2, capped at 1, so smooth gradients (where reuse makes bands)
    // barely trade and busy texture trades freely. 0 = no masking.
    float rdoMaskRef = 0.0f;
};

struct BlockInput {
    float r[16], g[16], b[16], a[16];
    // Dither noise per pixel in [-0.5, 0.5), one set per channel. May be null.
    const float* noiseCo = nullptr;
    const float* noiseCg = nullptr;
    const float* noiseY = nullptr;
    const float* noiseA = nullptr;
    bool hasAlpha = false;
    // Already written blocks to the left in this row, for size optimisation:
    // prev points at the block just before this one; prevCount blocks are valid.
    const uint8_t* prev = nullptr;
    int prevCount = 0;
};

// 16 bytes: BC4-style alpha block holding Y, then BC1 colour block holding Co, Cg, scale.
void encodeHapQBlock(const BlockInput& in, const BlockSettings& s, uint8_t out[16]);

// 8 bytes: RGTC1/BC4 block for 16 values in 0..1. weights may be null.
void encodeBC4Block(const float v[16], const float* weights, int radius, uint8_t out[8]);

// Size optimisation for a finished BC4 block: may swap it for one that reuses
// bytes of the prevCount blocks before it (stride bytes apart, prev = the one
// just before) when the error it adds is worth the bytes snappy saves.
// clean: the values to measure error against (no dither).
void rdoBC4Block(uint8_t blk[8], const float clean[16], const float* weights, float errScale, float lambda,
                 const uint8_t* prev, int stride, int prevCount);

} // namespace ghap
