#include "frame_encoder.h"

#include <algorithm>
#include <execution>
#include <numeric>

#include "bluenoise.h"
extern "C" {
#include "hap.h"
}

namespace ghap {

const char* formatName(HapFormat f) {
    return f == HapFormat::HapQAlpha ? "Hap Q Alpha" : "Hap Q";
}
const char* formatFourCC(HapFormat f) {
    return f == HapFormat::HapQAlpha ? "HapM" : "HapY";
}

BlockSettings blockSettingsFor(const EncoderSettings& s) {
    if (s.blockOverride) return *s.blockOverride;
    BlockSettings b;
    switch (s.quality) {
    case Quality::Fast: b.colorRadius = 1; b.alphaRadius = 3; b.scaleTries = 1; break;
    case Quality::Normal: b.colorRadius = 1; b.alphaRadius = 5; b.scaleTries = 2; break;
    case Quality::High: b.colorRadius = 1; b.alphaRadius = 8; b.scaleTries = 4; break;
    case Quality::Max: b.colorRadius = 2; b.alphaRadius = 10; b.scaleTries = 4; break;
    }
    b.ditherChroma = b.ditherLuma = b.ditherAlpha = s.dither;
    b.alphaWeighted = s.format == HapFormat::HapQAlpha;
    return b;
}

void encodeTextures(const FrameView& src, const EncoderSettings& s, int frameIndex, EncodedTextures& out) {
    const int tw = (src.width + 3) & ~3, th = (src.height + 3) & ~3;
    const int bw = tw / 4, bh = th / 4;
    const bool alpha = s.format == HapFormat::HapQAlpha;
    out.texWidth = tw;
    out.texHeight = th;
    out.ycocg.resize(size_t(bw) * bh * 16);
    out.alpha.resize(alpha ? size_t(bw) * bh * 8 : 0);

    const BlockSettings bs = blockSettingsFor(s);
    const bool dither = s.dither > 0;
    const int nf = s.animateDither ? frameIndex : 0;

    std::vector<int> rows(bh);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int by) {
        BlockInput in;
        float nco[16], ncg[16], ny[16], na[16];
        in.hasAlpha = alpha;
        if (dither) { in.noiseCo = nco; in.noiseCg = ncg; in.noiseY = ny; in.noiseA = na; }
        for (int bx = 0; bx < bw; ++bx) {
            for (int i = 0; i < 16; ++i) {
                int x = std::min(bx * 4 + (i & 3), src.width - 1);
                int y = std::min(by * 4 + (i >> 2), src.height - 1);
                const uint16_t* p = src.rgba + size_t(y) * src.stride + size_t(x) * 4;
                in.r[i] = p[0] * (1.0f / 65535.0f);
                in.g[i] = p[1] * (1.0f / 65535.0f);
                in.b[i] = p[2] * (1.0f / 65535.0f);
                in.a[i] = p[3] * (1.0f / 65535.0f);
                if (dither) {
                    int px = bx * 4 + (i & 3), py = by * 4 + (i >> 2);
                    nco[i] = blueNoiseAt(px, py, 0, nf);
                    ncg[i] = blueNoiseAt(px, py, 1, nf);
                    ny[i] = blueNoiseAt(px, py, 2, nf);
                    na[i] = blueNoiseAt(px, py, 3, nf);
                }
            }
            size_t b = size_t(by) * bw + bx;
            encodeHapQBlock(in, bs, out.ycocg.data() + b * 16);
            if (alpha) {
                float a[16];
                for (int i = 0; i < 16; ++i) {
                    float v = in.a[i];
                    if (dither) {
                        float fade = std::clamp(std::min(v, 1.0f - v) * 255.0f, 0.0f, 1.0f);
                        v = std::clamp(v + na[i] * bs.ditherAlpha / 255.0f * fade, 0.0f, 1.0f);
                    }
                    a[i] = v;
                }
                encodeBC4Block(a, nullptr, bs.alphaRadius, out.alpha.data() + b * 8);
            }
        }
    });
}

static unsigned autoChunks(size_t bytes) {
    // Roughly 256 KB per chunk lets decoders spread a frame over threads
    // without making chunks so small that snappy loses its window.
    return unsigned(std::clamp<size_t>(bytes / (256 * 1024), 1, 64));
}

bool packHapFrame(const EncodedTextures& tex, const EncoderSettings& s, std::vector<uint8_t>& frame) {
    const bool alpha = s.format == HapFormat::HapQAlpha;
    unsigned count = alpha ? 2 : 1;
    const void* buffers[2] = {tex.ycocg.data(), tex.alpha.data()};
    unsigned long lengths[2] = {(unsigned long)tex.ycocg.size(), (unsigned long)tex.alpha.size()};
    unsigned formats[2] = {HapTextureFormat_YCoCg_DXT5, HapTextureFormat_A_RGTC1};
    unsigned compressors[2] = {s.snappy ? (unsigned)HapCompressorSnappy : (unsigned)HapCompressorNone,
                               s.snappy ? (unsigned)HapCompressorSnappy : (unsigned)HapCompressorNone};
    unsigned chunks[2] = {s.chunks > 0 ? unsigned(s.chunks) : autoChunks(lengths[0]),
                          s.chunks > 0 ? unsigned(s.chunks) : autoChunks(lengths[1])};
    unsigned long maxLen = HapMaxEncodedLength(count, lengths, formats, chunks);
    if (!maxLen) return false;
    frame.resize(maxLen);
    unsigned long used = 0;
    unsigned r = HapEncode(count, buffers, lengths, formats, compressors, chunks, frame.data(), maxLen, &used);
    if (r != HapResult_No_Error) return false;
    frame.resize(used);
    return true;
}

static void serialDecode(HapDecodeWorkFunction fn, void* p, unsigned int count, void*) {
    std::vector<unsigned> idx(count);
    std::iota(idx.begin(), idx.end(), 0u);
    std::for_each(std::execution::par, idx.begin(), idx.end(), [&](unsigned i) { fn(p, i); });
}

bool unpackHapFrame(const uint8_t* data, size_t size, int width, int height, EncodedTextures& out, std::string* err) {
    unsigned count = 0;
    if (HapGetFrameTextureCount(data, (unsigned long)size, &count) != HapResult_No_Error) {
        if (err) *err = "not a Hap frame";
        return false;
    }
    out.texWidth = (width + 3) & ~3;
    out.texHeight = (height + 3) & ~3;
    size_t blocks = size_t(out.texWidth / 4) * (out.texHeight / 4);
    out.alpha.clear();
    for (unsigned i = 0; i < count; ++i) {
        unsigned fmt = 0;
        HapGetFrameTextureFormat(data, (unsigned long)size, i, &fmt);
        std::vector<uint8_t>* dst;
        if (fmt == HapTextureFormat_YCoCg_DXT5) { dst = &out.ycocg; dst->resize(blocks * 16); }
        else if (fmt == HapTextureFormat_A_RGTC1) { dst = &out.alpha; dst->resize(blocks * 8); }
        else {
            if (err) *err = "unsupported texture format (only Hap Q / Hap Q Alpha)";
            return false;
        }
        unsigned long used = 0;
        unsigned outFmt = 0;
        unsigned r = HapDecode(data, (unsigned long)size, i, serialDecode, nullptr, dst->data(),
                               (unsigned long)dst->size(), &used, &outFmt);
        if (r != HapResult_No_Error) {
            if (err) *err = "HapDecode failed";
            return false;
        }
    }
    return true;
}

} // namespace ghap
