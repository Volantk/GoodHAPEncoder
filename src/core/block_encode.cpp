#include "block_encode.h"

#include <algorithm>

#include "block_decode.h"
#include <cmath>
#include <cstring>

namespace ghap {
namespace {

constexpr float kOffset = 128.0f / 255.0f; // Hap Q chroma bias (shader: 0.50196078)

struct Tables {
    float r5[32], g6[64];
    Tables() {
        for (int i = 0; i < 32; ++i) r5[i] = float((i << 3) | (i >> 2)) / 255.0f;
        for (int i = 0; i < 64; ++i) g6[i] = float((i << 2) | (i >> 4)) / 255.0f;
    }
};
const Tables T;

inline int nearest5(float v) {
    int i = std::clamp(int(v * 31.0f + 0.5f), 0, 31);
    float best = std::fabs(T.r5[i] - v);
    for (int j = std::max(0, i - 1); j <= std::min(31, i + 1); ++j)
        if (std::fabs(T.r5[j] - v) < best) { best = std::fabs(T.r5[j] - v); i = j; }
    return i;
}
inline int nearest6(float v) {
    int i = std::clamp(int(v * 63.0f + 0.5f), 0, 63);
    float best = std::fabs(T.g6[i] - v);
    for (int j = std::max(0, i - 1); j <= std::min(63, i + 1); ++j)
        if (std::fabs(T.g6[j] - v) < best) { best = std::fabs(T.g6[j] - v); i = j; }
    return i;
}

// ---------------------------------------------------------------------------
// BC4 / DXT5 alpha

struct BC4Result {
    float err;
    int a0, a1;
    uint8_t idx[16];
};

// Palette in index order for the given endpoints (byte values).
inline void bc4Palette(int a0, int a1, float p[8]) {
    p[0] = a0 / 255.0f;
    p[1] = a1 / 255.0f;
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) p[i + 1] = ((7 - i) * a0 + i * a1) / (7.0f * 255.0f);
    } else {
        for (int i = 1; i <= 4; ++i) p[i + 1] = ((5 - i) * a0 + i * a1) / (5.0f * 255.0f);
        p[6] = 0.0f;
        p[7] = 1.0f;
    }
}

// w must point at 16 weights. Stops early once the error reaches bound.
inline float bc4Eval(const float v[16], const float* w, int a0, int a1, uint8_t* idx, float bound) {
    float p[8];
    bc4Palette(a0, a1, p);
    float err = 0;
    for (int i = 0; i < 16; ++i) {
        float best = 1e30f;
        int bi = 0;
        for (int k = 0; k < 8; ++k) {
            float d = p[k] - v[i];
            d *= d;
            if (d < best) { best = d; bi = k; }
        }
        err += best * w[i];
        if (idx) idx[i] = uint8_t(bi);
        else if (err >= bound) return err;
    }
    return err;
}

void bc4Search(const float v[16], const float* wIn, int radius, BC4Result& r) {
    float ones[16];
    const float* w = wIn;
    if (!w) { std::fill(ones, ones + 16, 1.0f); w = ones; }
    float lo = 1, hi = 0, lo6 = 1, hi6 = 0;
    bool any6 = false;
    for (int i = 0; i < 16; ++i) {
        lo = std::min(lo, v[i]);
        hi = std::max(hi, v[i]);
        if (v[i] > 1.5f / 255.0f && v[i] < 253.5f / 255.0f) {
            lo6 = std::min(lo6, v[i]);
            hi6 = std::max(hi6, v[i]);
            any6 = true;
        }
    }
    r.err = 1e30f;
    auto consider = [&](int a0, int a1) {
        float e = bc4Eval(v, w, a0, a1, nullptr, r.err);
        if (e < r.err) { r.err = e; r.a0 = a0; r.a1 = a1; }
    };
    // 8-level mode: a0 > a1
    int ilo = int(std::floor(lo * 255.0f)), ihi = int(std::ceil(hi * 255.0f));
    for (int a0 = std::max(0, ihi - radius); a0 <= std::min(255, ihi + radius); ++a0)
        for (int a1 = std::max(0, ilo - radius); a1 <= std::min(255, ilo + radius); ++a1)
            if (a0 > a1) consider(a0, a1);
    // flat
    for (int a = std::max(0, ilo - 1); a <= std::min(255, ihi + 1); ++a)
        if (ihi - ilo <= 2) consider(a, a);
    // 6-level mode with explicit 0 and 1: worth it when the block holds extremes
    if (any6 && (lo < 1.5f / 255.0f || hi > 253.5f / 255.0f)) {
        int jlo = int(std::floor(lo6 * 255.0f)), jhi = int(std::ceil(hi6 * 255.0f));
        for (int a0 = std::max(0, jlo - radius); a0 <= std::min(255, jlo + radius); ++a0)
            for (int a1 = std::max(0, jhi - radius); a1 <= std::min(255, jhi + radius); ++a1)
                if (a0 <= a1) consider(a0, a1);
    }
    bc4Eval(v, w, r.a0, r.a1, r.idx, 1e30f);
}

void bc4Pack(const BC4Result& r, uint8_t out[8]) {
    out[0] = uint8_t(r.a0);
    out[1] = uint8_t(r.a1);
    uint64_t bits = 0;
    uint8_t idx[16];
    std::memcpy(idx, r.idx, 16);
    if (r.a0 == r.a1)
        for (auto& i : idx)
            if (i < 6) i = 0; // all interior levels equal a0 anyway
    for (int i = 0; i < 16; ++i) bits |= uint64_t(idx[i] & 7) << (3 * i);
    for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(bits >> (8 * i));
}

// ---------------------------------------------------------------------------
// BC1 colour part holding scaled Co (5 bit, red) and Cg (6 bit, green)

struct ColorFit {
    float err;
    int r0, g0, r1, g1;
    uint8_t step[16]; // 0 = e0, 1 = 2/3 e0 + 1/3 e1, 2 = 1/3 e0 + 2/3 e1, 3 = e1
};

struct ColorTargets {
    float x[16], y[16], w[16];
    float wx, wy; // metric weights (stored space -> RGB squared error)
};

// Stops early once the error reaches bound (unless steps are wanted).
inline float colorEval(const ColorTargets& t, int r0, int g0, int r1, int g1, uint8_t* step, float bound) {
    float x0 = T.r5[r0], y0 = T.g6[g0], x1 = T.r5[r1], y1 = T.g6[g1];
    float px[4] = {x0, (2 * x0 + x1) / 3, (x0 + 2 * x1) / 3, x1};
    float py[4] = {y0, (2 * y0 + y1) / 3, (y0 + 2 * y1) / 3, y1};
    float err = 0;
    for (int i = 0; i < 16; ++i) {
        float best = 1e30f;
        int bk = 0;
        for (int k = 0; k < 4; ++k) {
            float dx = px[k] - t.x[i], dy = py[k] - t.y[i];
            float d = t.wx * dx * dx + t.wy * dy * dy;
            if (d < best) { best = d; bk = k; }
        }
        err += best * t.w[i];
        if (step) step[i] = uint8_t(bk);
        else if (err >= bound) return err;
    }
    return err;
}

// Least squares endpoints for fixed steps, per channel.
bool colorLsq(const ColorTargets& t, const uint8_t step[16], float& x0, float& y0, float& x1, float& y1) {
    float aa = 0, ab = 0, bb = 0, ax = 0, bx = 0, ay = 0, by = 0;
    for (int i = 0; i < 16; ++i) {
        float b = step[i] / 3.0f, a = 1.0f - b, w = t.w[i];
        aa += w * a * a; ab += w * a * b; bb += w * b * b;
        ax += w * a * t.x[i]; bx += w * b * t.x[i];
        ay += w * a * t.y[i]; by += w * b * t.y[i];
    }
    float det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-8f) return false;
    float inv = 1.0f / det;
    x0 = (ax * bb - bx * ab) * inv; x1 = (bx * aa - ax * ab) * inv;
    y0 = (ay * bb - by * ab) * inv; y1 = (by * aa - ay * ab) * inv;
    return true;
}

void colorFit(const ColorTargets& t, int radius, ColorFit& f) {
    // Weighted PCA in metric space.
    float sw = 0, mx = 0, my = 0;
    for (int i = 0; i < 16; ++i) { sw += t.w[i]; mx += t.w[i] * t.x[i]; my += t.w[i] * t.y[i]; }
    mx /= sw; my /= sw;
    float sx = std::sqrt(t.wx), sy = std::sqrt(t.wy);
    float cxx = 0, cxy = 0, cyy = 0;
    for (int i = 0; i < 16; ++i) {
        float dx = (t.x[i] - mx) * sx, dy = (t.y[i] - my) * sy;
        cxx += t.w[i] * dx * dx; cxy += t.w[i] * dx * dy; cyy += t.w[i] * dy * dy;
    }
    float ax, ay;
    {
        float tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
        float l = tr * 0.5f + std::sqrt(std::max(0.0f, tr * tr * 0.25f - det));
        if (std::fabs(cxy) > 1e-12f) { ax = l - cyy; ay = cxy; }
        else if (cxx >= cyy) { ax = 1; ay = 0; }
        else { ax = 0; ay = 1; }
        float n = std::sqrt(ax * ax + ay * ay);
        ax /= n; ay /= n;
    }
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < 16; ++i) {
        float p = (t.x[i] - mx) * sx * ax + (t.y[i] - my) * sy * ay;
        tmin = std::min(tmin, p); tmax = std::max(tmax, p);
    }
    float x0 = mx + tmin * ax / sx, y0 = my + tmin * ay / sy;
    float x1 = mx + tmax * ax / sx, y1 = my + tmax * ay / sy;

    f.err = 1e30f;
    auto tryEndpoints = [&](int r0, int g0, int r1, int g1) {
        float e = colorEval(t, r0, g0, r1, g1, nullptr, f.err);
        if (e < f.err) { f.err = e; f.r0 = r0; f.g0 = g0; f.r1 = r1; f.g1 = g1; }
    };
    tryEndpoints(nearest5(x0), nearest6(y0), nearest5(x1), nearest6(y1));
    tryEndpoints(nearest5(mx), nearest6(my), nearest5(mx), nearest6(my));

    // Iterative least squares refinement.
    uint8_t step[16];
    for (int it = 0; it < 3; ++it) {
        colorEval(t, f.r0, f.g0, f.r1, f.g1, step, 1e30f);
        if (!colorLsq(t, step, x0, y0, x1, y1)) break;
        float before = f.err;
        tryEndpoints(nearest5(std::clamp(x0, 0.f, 1.f)), nearest6(std::clamp(y0, 0.f, 1.f)),
                     nearest5(std::clamp(x1, 0.f, 1.f)), nearest6(std::clamp(y1, 0.f, 1.f)));
        if (f.err >= before) break;
    }

    // Local exhaustive search around the best quantised endpoints. Repeat while
    // the centre keeps moving, so a radius of 1 can still walk further.
    for (int pass = 0; pass < 4 && radius > 0; ++pass) {
        const int cr0 = f.r0, cg0 = f.g0, cr1 = f.r1, cg1 = f.g1;
        for (int dr0 = -radius; dr0 <= radius; ++dr0) {
            int r0 = cr0 + dr0;
            if (r0 < 0 || r0 > 31) continue;
            for (int dg0 = -radius; dg0 <= radius; ++dg0) {
                int g0 = cg0 + dg0;
                if (g0 < 0 || g0 > 63) continue;
                for (int dr1 = -radius; dr1 <= radius; ++dr1) {
                    int r1 = cr1 + dr1;
                    if (r1 < 0 || r1 > 31) continue;
                    for (int dg1 = -radius; dg1 <= radius; ++dg1) {
                        int g1 = cg1 + dg1;
                        if (g1 < 0 || g1 > 63) continue;
                        tryEndpoints(r0, g0, r1, g1);
                    }
                }
            }
        }
        if (f.r0 == cr0 && f.g0 == cg0 && f.r1 == cr1 && f.g1 == cg1) break;
    }
    colorEval(t, f.r0, f.g0, f.r1, f.g1, f.step, 1e30f);
}

void colorPack(const ColorFit& f, int scale, uint8_t out[8]) {
    int b = scale - 1; // blue 5-bit: 0,1,2,3 -> expanded 0,8,16,24 -> scale 1..4
    uint16_t c0 = uint16_t((f.r0 << 11) | (f.g0 << 5) | b);
    uint16_t c1 = uint16_t((f.r1 << 11) | (f.g1 << 5) | b);
    uint8_t step[16];
    std::memcpy(step, f.step, 16);
    if (c0 < c1) {
        std::swap(c0, c1);
        for (auto& s : step) s = uint8_t(3 - s);
    }
    static const uint8_t toIndex[4] = {0, 2, 3, 1};
    uint32_t bits = 0;
    if (c0 != c1)
        for (int i = 0; i < 16; ++i) bits |= uint32_t(toIndex[step[i]]) << (2 * i);
    out[0] = uint8_t(c0); out[1] = uint8_t(c0 >> 8);
    out[2] = uint8_t(c1); out[3] = uint8_t(c1 >> 8);
    for (int i = 0; i < 4; ++i) out[4 + i] = uint8_t(bits >> (8 * i));
}

// Fade dither near 0 and 1 so pure black, white and opaque stay exact.
inline float edgeFade(float v) {
    return std::clamp(std::min(v, 1.0f - v) * 255.0f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Size optimisation (RDO for snappy)
//
// Hap's second stage is snappy, which only shrinks data that repeats: a run of
// 4+ bytes seen within the last 64 KB costs ~2-3 bytes instead of itself. Each
// block half is 8 bytes: endpoints, then index bits. After the best fit is
// found, these candidates reuse bytes of blocks just to the left in the same
// row (deterministic, and rows encode in parallel):
//   whole half copied            ~2 bytes
//   index bits copied, own endpoints refit to them   ~4-6 bytes
//   colour endpoints copied, own indices             ~6 bytes
//   flat luma block (zero index bits, which repeat everywhere)  ~3 bytes
// The candidate with the lowest error + lambda * bytes wins. Byte costs are
// estimates of what snappy pays; the real saving is measured, not assumed.

constexpr float kCostNew = 8, kCostCopy = 2, kCostBC4Index = 4, kCostColorPart = 6, kCostFlat = 3;

inline float bc4Err(const uint8_t blk[8], const float v[16], const float* w) {
    float d[16];
    decodeBC4Block(blk, d);
    float e = 0;
    for (int i = 0; i < 16; ++i) e += (d[i] - v[i]) * (d[i] - v[i]) * (w ? w[i] : 1.0f);
    return e;
}

// Squared RGB error of a colour half against the block's Co/Cg, assuming luma
// absorbs the mean (see the Y target below): 2 dCo^2 + 8/3 dCg^2.
inline float chromaErr(const uint8_t half[8], const float Co[16], const float Cg[16], const float w[16]) {
    float c[48];
    decodeBC1Block(half, c, true);
    float e = 0;
    for (int i = 0; i < 16; ++i) {
        float s = c[i * 3 + 2] * (255.0f / 8.0f) + 1.0f;
        float dco = (c[i * 3] - kOffset) / s - Co[i], dcg = (c[i * 3 + 1] - kOffset) / s - Cg[i];
        e += w[i] * (2.0f * dco * dco + (8.0f / 3.0f) * dcg * dcg);
    }
    return e;
}

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

void rdoColorHalf(uint8_t half[8], const ColorTargets& t, int scale, const float Co[16], const float Cg[16], const float w[16],
                  float lambda, const uint8_t* prevBlock, int prevCount) {
    float bestJ = chromaErr(half, Co, Cg, w) + lambda * kCostNew;
    uint8_t best[8];
    std::memcpy(best, half, 8);
    auto consider = [&](const uint8_t cand[8], float cost) {
        float j = chromaErr(cand, Co, Cg, w) + lambda * cost;
        if (j < bestJ) { bestJ = j; std::memcpy(best, cand, 8); }
    };
    static const uint8_t idxToStep[4] = {0, 3, 1, 2};
    for (int k = 0; k < prevCount; ++k) {
        const uint8_t* c = prevBlock - 16 * k + 8;
        consider(c, kCostCopy);

        // Their endpoints, our best indices for them.
        {
            uint8_t cand[8];
            std::memcpy(cand, c, 4);
            uint32_t bits = 0;
            if (rd16(c) != rd16(c + 2)) {
                float pal[48];
                uint8_t probe[8];
                std::memcpy(probe, c, 4);
                probe[4] = 0xE4; probe[5] = probe[6] = probe[7] = 0; // indices 0,1,2,3 for the first four pixels
                decodeBC1Block(probe, pal, true);
                float s = pal[2] * (255.0f / 8.0f) + 1.0f;
                for (int i = 0; i < 16; ++i) {
                    float bestD = 1e30f;
                    int bi = 0;
                    for (int p = 0; p < 4; ++p) {
                        float dco = (pal[p * 3] - kOffset) / s - Co[i], dcg = (pal[p * 3 + 1] - kOffset) / s - Cg[i];
                        float d = 2.0f * dco * dco + (8.0f / 3.0f) * dcg * dcg;
                        if (d < bestD) { bestD = d; bi = p; }
                    }
                    bits |= uint32_t(bi) << (2 * i);
                }
            }
            for (int i = 0; i < 4; ++i) cand[4 + i] = uint8_t(bits >> (8 * i));
            consider(cand, kCostColorPart);
        }

        // Their index bits, our endpoints refit to them (at our scale).
        {
            uint32_t bits = uint32_t(c[4] | (c[5] << 8) | (c[6] << 16) | (uint32_t(c[7]) << 24));
            uint8_t step[16];
            for (int i = 0; i < 16; ++i) step[i] = idxToStep[(bits >> (2 * i)) & 3];
            float x0, y0, x1, y1;
            if (colorLsq(t, step, x0, y0, x1, y1)) {
                int b = scale - 1;
                uint16_t c0 = uint16_t((nearest5(std::clamp(x0, 0.f, 1.f)) << 11) | (nearest6(std::clamp(y0, 0.f, 1.f)) << 5) | b);
                uint16_t c1 = uint16_t((nearest5(std::clamp(x1, 0.f, 1.f)) << 11) | (nearest6(std::clamp(y1, 0.f, 1.f)) << 5) | b);
                // Keep c0 > c1: some old GPUs decode DXT5 colour like DXT1, where c0 <= c1 means 3-colour mode.
                if (c0 > c1) {
                    uint8_t cand[8] = {uint8_t(c0), uint8_t(c0 >> 8), uint8_t(c1), uint8_t(c1 >> 8), c[4], c[5], c[6], c[7]};
                    consider(cand, kCostColorPart);
                }
            }
        }
    }
    std::memcpy(half, best, 8);
}

} // namespace

void rdoBC4Block(uint8_t blk[8], const float v[16], const float* w, float errScale, float lambda, const uint8_t* prev, int stride,
                 int prevCount) {
    float bestJ = errScale * bc4Err(blk, v, w) + lambda * kCostNew;
    uint8_t best[8];
    std::memcpy(best, blk, 8);
    auto consider = [&](const uint8_t cand[8], float cost) {
        float j = errScale * bc4Err(cand, v, w) + lambda * cost;
        if (j < bestJ) { bestJ = j; std::memcpy(best, cand, 8); }
    };
    {
        float sw = 0, sv = 0;
        for (int i = 0; i < 16; ++i) { float wi = w ? w[i] : 1.0f; sw += wi; sv += wi * v[i]; }
        uint8_t a = uint8_t(std::clamp(int(sv / sw * 255.0f + 0.5f), 0, 255));
        uint8_t flat[8] = {a, a, 0, 0, 0, 0, 0, 0};
        consider(flat, kCostFlat);
    }
    for (int k = 0; k < prevCount; ++k) {
        const uint8_t* c = prev - stride * k;
        consider(c, kCostCopy);
        // Their index bits, our endpoints: least squares in 8-level mode.
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(c[2 + i]) << (8 * i);
        float aa = 0, ab = 0, bb = 0, av = 0, bv = 0;
        for (int i = 0; i < 16; ++i) {
            int idx = int((bits >> (3 * i)) & 7);
            float f1 = idx == 0 ? 0.0f : idx == 1 ? 1.0f : (idx - 1) / 7.0f; // weight of a1
            float f0 = 1.0f - f1, wi = w ? w[i] : 1.0f;
            aa += wi * f0 * f0; ab += wi * f0 * f1; bb += wi * f1 * f1;
            av += wi * f0 * v[i]; bv += wi * f1 * v[i];
        }
        float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-8f) continue;
        int a0 = std::clamp(int((av * bb - bv * ab) / det * 255.0f + 0.5f), 0, 255);
        int a1 = std::clamp(int((bv * aa - av * ab) / det * 255.0f + 0.5f), 0, 255);
        if (a0 <= a1) continue; // would switch to 6-level mode and change the meaning of the indices
        uint8_t cand[8] = {uint8_t(a0), uint8_t(a1), c[2], c[3], c[4], c[5], c[6], c[7]};
        consider(cand, kCostBC4Index);
    }
    std::memcpy(blk, best, 8);
}

void encodeBC4Block(const float v[16], const float* weights, int radius, uint8_t out[8]) {
    BC4Result r;
    bc4Search(v, weights, radius, r);
    bc4Pack(r, out);
}

void encodeHapQBlock(const BlockInput& in, const BlockSettings& s, uint8_t out[16]) {
    float Y[16], Co[16], Cg[16], w[16];
    float maxC = 0;
    for (int i = 0; i < 16; ++i) {
        float r = in.r[i], g = in.g[i], b = in.b[i];
        Y[i] = 0.25f * r + 0.5f * g + 0.25f * b;
        Co[i] = 0.5f * (r - b);
        Cg[i] = 0.5f * g - 0.25f * (r + b);
        maxC = std::max(maxC, std::max(std::fabs(Co[i]), std::fabs(Cg[i])));
        w[i] = (in.hasAlpha && s.alphaWeighted) ? std::max(in.a[i], 1.0f / 16.0f) : 1.0f;
    }

    // Largest scale that keeps every chroma value representable (stored range 0..1).
    int maxScale = 1;
    for (int sc = 4; sc >= 1; --sc)
        if (maxC * sc <= 127.0f / 255.0f + 0.5f / 255.0f) { maxScale = sc; break; }
    int minScale = std::max(1, maxScale - std::max(1, s.scaleTries) + 1);

    ColorFit best;
    best.err = 1e30f;
    int bestScale = 1;
    ColorTargets bestTargets;
    for (int sc = maxScale; sc >= minScale; --sc) {
        ColorTargets t;
        float inv = 1.0f / float(sc * sc);
        t.wx = 2.0f * inv;
        t.wy = (8.0f / 3.0f) * inv;
        const float ax = s.ditherChroma / (31.0f * 3.0f), ay = s.ditherChroma / (63.0f * 3.0f);
        for (int i = 0; i < 16; ++i) {
            t.x[i] = Co[i] * sc + kOffset;
            t.y[i] = Cg[i] * sc + kOffset;
            if (in.noiseCo) t.x[i] += in.noiseCo[i] * ax;
            if (in.noiseCg) t.y[i] += in.noiseCg[i] * ay;
            t.x[i] = std::clamp(t.x[i], 0.0f, 1.0f);
            t.y[i] = std::clamp(t.y[i], 0.0f, 1.0f);
            t.w[i] = w[i];
        }
        ColorFit f;
        colorFit(t, s.colorRadius, f);
        if (f.err < best.err) { best = f; bestScale = sc; bestTargets = t; }
    }
    uint8_t color[8];
    colorPack(best, bestScale, color);
    float lambda = s.rdoLambda;
    if (lambda > 0 && s.rdoMaskRef > 0) {
        float m[3] = {}, v = 0;
        for (int i = 0; i < 16; ++i) { m[0] += in.r[i]; m[1] += in.g[i]; m[2] += in.b[i]; }
        for (auto& x : m) x /= 16;
        for (int i = 0; i < 16; ++i) {
            float dr = in.r[i] - m[0], dg = in.g[i] - m[1], db = in.b[i] - m[2];
            v += dr * dr + dg * dg + db * db;
        }
        float sigma = std::sqrt(v / 48.0f);
        lambda *= std::min(1.0f, (sigma / s.rdoMaskRef) * (sigma / s.rdoMaskRef));
    }
    const bool rdo = lambda > 0 && in.prev && in.prevCount > 0;
    const int window = std::min(in.prevCount, s.rdoWindow);
    if (rdo) {
        // Score candidates against the (dithered) targets the best fit was made
        // for; otherwise reuse would win just by removing the dither.
        float coT[16], cgT[16];
        for (int i = 0; i < 16; ++i) {
            coT[i] = (bestTargets.x[i] - kOffset) / bestScale;
            cgT[i] = (bestTargets.y[i] - kOffset) / bestScale;
        }
        rdoColorHalf(color, bestTargets, bestScale, coT, cgT, w, lambda, in.prev, window);
    }

    // Luma target compensates the decoded chroma error: for RGB = Y + f(Co, Cg)
    // the least squares Y is Y + (Cg' - Cg) / 3.
    float yt[16], yw[16];
    {
        float c[48];
        decodeBC1Block(color, c, true);
        for (int i = 0; i < 16; ++i) {
            float sc = c[i * 3 + 2] * (255.0f / 8.0f) + 1.0f;
            float cgDecoded = (c[i * 3 + 1] - kOffset) / sc;
            float v = Y[i] + (cgDecoded - Cg[i]) / 3.0f;
            if (in.noiseY) v += in.noiseY[i] * s.ditherLuma / 255.0f * edgeFade(v);
            yt[i] = std::clamp(v, 0.0f, 1.0f);
            yw[i] = w[i];
        }
    }
    encodeBC4Block(yt, yw, s.alphaRadius, out);
    // A luma error shows in all three channels, hence errScale 3.
    if (rdo) rdoBC4Block(out, yt, yw, 3.0f, lambda, in.prev, 16, window);
    std::memcpy(out + 8, color, 8);
}

} // namespace ghap
