#pragma once
#include <cstdint>

namespace ghap {

// Float decoders modelled on GPU decode (no 8-bit rounding of interpolated
// values), followed by the Hap Q shader. Used for metrics and previews.

void decodeBC4Block(const uint8_t in[8], float out[16]);

// Hap Q block -> RGB (16 * 3 floats, interleaved, unclamped).
void decodeHapQBlock(const uint8_t in[16], float rgb[48]);

// Plain DXT1 / BC1 (4-colour mode only when c0 > c1). rgb = 16 * 3 floats.
void decodeBC1Block(const uint8_t in[8], float rgb[48], bool forceFourColor);

// Whole texture helpers. Width/height are the padded texture size (multiples of 4).
// out is RGBA float, width*height*4.
void decodeHapQTexture(const uint8_t* tex, int width, int height, float* outRGBA);
void decodeHapQAlphaTextures(const uint8_t* ycocg, const uint8_t* alpha, int width, int height, float* outRGBA);

} // namespace ghap
