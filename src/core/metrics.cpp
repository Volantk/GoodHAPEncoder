#include "metrics.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <numeric>

#include "block_decode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace ghap {

ImageF imageFromFrame(const Frame16& f) {
    ImageF im{f.width, f.height, std::vector<float>(size_t(f.width) * f.height * 3)};
    for (size_t i = 0; i < size_t(f.width) * f.height; ++i)
        for (int k = 0; k < 3; ++k) im.rgb[i * 3 + k] = f.rgba[i * 4 + k] / 65535.f;
    return im;
}

ImageF imageFromTextures(const EncodedTextures& t, int w, int h) {
    std::vector<float> rgba(size_t(t.texWidth) * t.texHeight * 4);
    decodeHapQAlphaTextures(t.ycocg.data(), t.alpha.empty() ? nullptr : t.alpha.data(), t.texWidth, t.texHeight, rgba.data());
    ImageF im{w, h, std::vector<float>(size_t(w) * h * 3)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) im.rgb[(size_t(y) * w + x) * 3 + k] = rgba[(size_t(y) * t.texWidth + x) * 4 + k];
    return im;
}

static void blur(const ImageF& in, ImageF& out, float sigma) {
    int rad = int(std::ceil(sigma * 3));
    std::vector<float> k(2 * rad + 1);
    float s = 0;
    for (int i = -rad; i <= rad; ++i) s += k[i + rad] = std::exp(-i * i / (2 * sigma * sigma));
    for (auto& v : k) v /= s;
    ImageF tmp = in;
    out = in;
    std::vector<int> rows(in.h);
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y) {
        for (int x = 0; x < in.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float a = 0;
                for (int i = -rad; i <= rad; ++i) a += k[i + rad] * in.rgb[(size_t(y) * in.w + std::clamp(x + i, 0, in.w - 1)) * 3 + c];
                tmp.rgb[(size_t(y) * in.w + x) * 3 + c] = a;
            }
    });
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int y) {
        for (int x = 0; x < in.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float a = 0;
                for (int i = -rad; i <= rad; ++i) a += k[i + rad] * tmp.rgb[(size_t(std::clamp(y + i, 0, in.h - 1)) * in.w + x) * 3 + c];
                out.rgb[(size_t(y) * in.w + x) * 3 + c] = a;
            }
    });
}

static double psnr(const ImageF& a, const ImageF& b) {
    double se = 0;
    for (size_t i = 0; i < a.rgb.size(); ++i) { double d = double(a.rgb[i]) - b.rgb[i]; se += d * d; }
    double mse = se / a.rgb.size();
    return mse <= 0 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}

Metrics measure(const ImageF& src, const ImageF& dec) {
    ImageF d = dec, d8 = dec;
    for (auto& v : d.rgb) v = std::clamp(v, 0.f, 1.f);
    for (auto& v : d8.rgb) v = std::round(std::clamp(v, 0.f, 1.f) * 255.f) / 255.f;
    ImageF bs, bd;
    blur(src, bs, 2.5f);
    blur(d8, bd, 2.5f);
    return {psnr(src, d), psnr(src, d8), psnr(bs, bd)};
}

bool ffmpegHapQReference(const Frame16& src, EncodedTextures& out) {
    const AVCodec* codec = avcodec_find_encoder_by_name("hap");
    if (!codec) return false;
    const int tw = (src.width + 3) & ~3, th = (src.height + 3) & ~3;
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = tw;
    c->height = th;
    c->pix_fmt = AV_PIX_FMT_RGBA;
    c->time_base = {1, 25};
    av_opt_set(c->priv_data, "format", "hap_q", 0);
    av_opt_set(c->priv_data, "compressor", "none", 0);
    bool ok = false;
    AVFrame* f = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_open2(c, codec, nullptr) >= 0) {
        f->format = AV_PIX_FMT_RGBA;
        f->width = tw;
        f->height = th;
        av_frame_get_buffer(f, 0);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                const uint16_t* p = &src.rgba[(size_t(std::min(y, src.height - 1)) * src.width + std::min(x, src.width - 1)) * 4];
                uint8_t* o = f->data[0] + size_t(y) * f->linesize[0] + x * 4;
                for (int k = 0; k < 4; ++k) o[k] = uint8_t((p[k] * 255u + 32767u) / 65535u);
            }
        if (avcodec_send_frame(c, f) >= 0 && avcodec_send_frame(c, nullptr) >= 0 && avcodec_receive_packet(c, pkt) >= 0)
            ok = unpackHapFrame(pkt->data, pkt->size, tw, th, out);
    }
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&c);
    return ok;
}

} // namespace ghap
