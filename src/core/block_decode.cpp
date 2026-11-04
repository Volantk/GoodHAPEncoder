#include "block_decode.h"

namespace ghap {

void decodeBC4Block(const uint8_t in[8], float out[16]) {
    int a0 = in[0], a1 = in[1];
    float p[8];
    p[0] = a0 / 255.0f;
    p[1] = a1 / 255.0f;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) p[i + 1] = ((7 - i) * a0 + i * a1) / (7.0f * 255.0f);
    } else {
        for (int i = 1; i <= 4; ++i) p[i + 1] = ((5 - i) * a0 + i * a1) / (5.0f * 255.0f);
        p[6] = 0.0f;
        p[7] = 1.0f;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(in[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = p[(bits >> (3 * i)) & 7];
}

void decodeBC1Block(const uint8_t in[8], float rgb[48], bool forceFourColor) {
    uint16_t c0 = uint16_t(in[0] | (in[1] << 8)), c1 = uint16_t(in[2] | (in[3] << 8));
    auto expand = [](uint16_t c, float out[3]) {
        int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        out[0] = float((r << 3) | (r >> 2)) / 255.0f;
        out[1] = float((g << 2) | (g >> 4)) / 255.0f;
        out[2] = float((b << 3) | (b >> 2)) / 255.0f;
    };
    float p[4][3];
    expand(c0, p[0]);
    expand(c1, p[1]);
    for (int k = 0; k < 3; ++k) {
        if (forceFourColor || c0 > c1) {
            p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
            p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
        } else {
            p[2][k] = (p[0][k] + p[1][k]) / 2;
            p[3][k] = 0;
        }
    }
    uint32_t bits = uint32_t(in[4] | (in[5] << 8) | (in[6] << 16) | (uint32_t(in[7]) << 24));
    for (int i = 0; i < 16; ++i) {
        int idx = (bits >> (2 * i)) & 3;
        for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = p[idx][k];
    }
}

void decodeHapQBlock(const uint8_t in[16], float rgb[48]) {
    float y[16], c[48];
    decodeBC4Block(in, y);
    decodeBC1Block(in + 8, c, true);
    const float off = 0.50196078431373f;
    for (int i = 0; i < 16; ++i) {
        float scale = c[i * 3 + 2] * (255.0f / 8.0f) + 1.0f;
        float co = (c[i * 3 + 0] - off) / scale;
        float cg = (c[i * 3 + 1] - off) / scale;
        rgb[i * 3 + 0] = y[i] + co - cg;
        rgb[i * 3 + 1] = y[i] + cg;
        rgb[i * 3 + 2] = y[i] - co - cg;
    }
}

void decodeHapQAlphaTextures(const uint8_t* ycocg, const uint8_t* alpha, int width, int height, float* out) {
    int bw = width / 4, bh = height / 4;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            float rgb[48], a[16];
            size_t b = size_t(by) * bw + bx;
            decodeHapQBlock(ycocg + b * 16, rgb);
            if (alpha) decodeBC4Block(alpha + b * 8, a);
            for (int i = 0; i < 16; ++i) {
                float* o = out + ((size_t(by * 4 + i / 4) * width) + bx * 4 + i % 4) * 4;
                o[0] = rgb[i * 3]; o[1] = rgb[i * 3 + 1]; o[2] = rgb[i * 3 + 2];
                o[3] = alpha ? a[i] : 1.0f;
            }
        }
}

void decodeHapQTexture(const uint8_t* tex, int width, int height, float* out) {
    decodeHapQAlphaTextures(tex, nullptr, width, height, out);
}

} // namespace ghap
