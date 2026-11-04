// ghap_eval: test images, settings sweeps and metrics for Hap Q encoders.
//
//   ghap_eval gen <sky|dark|ramps|detail> <out.png> [w h]
//   ghap_eval sweep <source> [--frame N] [--dump dir]
//   ghap_eval metrics <source> <hap.mov> [--frame N] [--dump prefix]
//
// Metrics compare against the source at full precision, decoding Hap the way
// a GPU does (float interpolation + Hap Q shader):
//   psnr     float output
//   psnr8    output rounded to 8 bits (what an 8-bit display pipeline shows)
//   smooth8  psnr8 after a gaussian blur (sigma 2.5) of both images: errors that
//            survive blurring are banding, tints and blotches, not fine noise

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "block_decode.h"
#include "frame_encoder.h"
#include "media_reader.h"

using namespace ghap;

#ifndef GHAP_FFMPEG_EXE
#define GHAP_FFMPEG_EXE "ffmpeg"
#endif

struct ImageF {
    int w = 0, h = 0;
    std::vector<float> rgb; // interleaved 3
};

static bool writePng(const std::string& path, int w, int h, const std::vector<uint16_t>& rgba64) {
    std::string cmd = std::string("\"\"") + GHAP_FFMPEG_EXE + "\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba64le -s " +
                      std::to_string(w) + "x" + std::to_string(h) + " -i - -pix_fmt rgb48be \"" + path + "\"\"";
    FILE* p = _popen(cmd.c_str(), "wb");
    if (!p) return false;
    fwrite(rgba64.data(), 2, rgba64.size(), p);
    return _pclose(p) == 0;
}

static bool writePng(const std::string& path, const ImageF& im) {
    std::vector<uint16_t> buf(size_t(im.w) * im.h * 4);
    for (size_t i = 0; i < size_t(im.w) * im.h; ++i) {
        for (int k = 0; k < 3; ++k) buf[i * 4 + k] = uint16_t(std::clamp(im.rgb[i * 3 + k], 0.f, 1.f) * 65535.f + 0.5f);
        buf[i * 4 + 3] = 65535;
    }
    return writePng(path, im.w, im.h, buf);
}

// --- synthetic sources ------------------------------------------------------

static float smoothstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

static ImageF generate(const std::string& kind, int w, int h) {
    ImageF im{w, h, std::vector<float>(size_t(w) * h * 3)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float r = 0, g = 0, b = 0;
            if (kind == "sky") {
                float t = std::pow(v, 1.3f);
                r = 0.05f + 0.80f * t; g = 0.12f + 0.50f * t; b = 0.38f + 0.10f * t;
                float dx = (u - 0.68f) * w / h, dy = v - 0.72f;
                float d = std::sqrt(dx * dx + dy * dy);
                float glow = std::exp(-d * d * 9.f) * 0.45f + std::exp(-d * d * 120.f) * 0.3f;
                r += glow; g += glow * 0.75f; b += glow * 0.35f;
            } else if (kind == "dark") {
                float dx = (u - 0.5f) * w / h, dy = v - 0.5f;
                float d = std::sqrt(dx * dx + dy * dy);
                float l = 0.09f * std::exp(-d * d * 4.f);
                r = l * 1.0f; g = l * 0.75f; b = l * 1.25f + 0.004f * u;
            } else if (kind == "ramps") {
                int band = int(v * 9);
                static const float cols[8][3] = {{1, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 0}, {1, .55f, .2f}};
                if (band < 8) { r = u * cols[band][0]; g = u * cols[band][1]; b = u * cols[band][2]; }
                else { float l = 0.40f + 0.05f * u; r = l; g = l * 0.97f; b = l * 0.92f; } // narrow-range ramp
            } else if (kind == "detail") {
                float cx = u - 0.5f, cy = (v - 0.5f) * h / w;
                float zp = 0.5f + 0.5f * std::cos(2200.f * (cx * cx + cy * cy));
                float checker = float(((x >> 1) ^ (y >> 1)) & 1);
                float lines = (x % 7 == 0) ? 1.f : 0.f;
                if (u < 0.5f) { r = g = b = zp; }
                else if (v < 0.5f) { r = checker; g = 0.5f * checker + 0.25f; b = 1 - checker; }
                else { r = 0.2f + 0.6f * lines; g = 0.3f + 0.5f * smoothstep(0.5f, 1.f, u); b = 0.7f - 0.5f * lines; }
            }
            float* p = &im.rgb[(size_t(y) * w + x) * 3];
            p[0] = std::clamp(r, 0.f, 1.f); p[1] = std::clamp(g, 0.f, 1.f); p[2] = std::clamp(b, 0.f, 1.f);
        }
    return im;
}

// --- metrics ------------------------------------------------------------------

static void blur(const ImageF& in, ImageF& out, float sigma) {
    int rad = int(std::ceil(sigma * 3));
    std::vector<float> k(2 * rad + 1);
    float s = 0;
    for (int i = -rad; i <= rad; ++i) s += k[i + rad] = std::exp(-i * i / (2 * sigma * sigma));
    for (auto& v : k) v /= s;
    ImageF tmp = in;
    out = in;
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float a = 0;
                for (int i = -rad; i <= rad; ++i) a += k[i + rad] * in.rgb[(size_t(y) * in.w + std::clamp(x + i, 0, in.w - 1)) * 3 + c];
                tmp.rgb[(size_t(y) * in.w + x) * 3 + c] = a;
            }
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float a = 0;
                for (int i = -rad; i <= rad; ++i) a += k[i + rad] * tmp.rgb[(size_t(std::clamp(y + i, 0, in.h - 1)) * in.w + x) * 3 + c];
                out.rgb[(size_t(y) * in.w + x) * 3 + c] = a;
            }
}

static double psnr(const ImageF& a, const ImageF& b) {
    double se = 0;
    for (size_t i = 0; i < a.rgb.size(); ++i) { double d = double(a.rgb[i]) - b.rgb[i]; se += d * d; }
    double mse = se / a.rgb.size();
    return mse <= 0 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}

struct Metrics { double psnr, psnr8, smooth8; };

static Metrics measure(const ImageF& src, const ImageF& dec) {
    ImageF d = dec, d8 = dec;
    for (auto& v : d.rgb) v = std::clamp(v, 0.f, 1.f);
    for (auto& v : d8.rgb) v = std::round(std::clamp(v, 0.f, 1.f) * 255.f) / 255.f;
    ImageF bs, bd;
    blur(src, bs, 2.5f);
    blur(d8, bd, 2.5f);
    return {psnr(src, d), psnr(src, d8), psnr(bs, bd)};
}

static ImageF fromFrame(const Frame16& f) {
    ImageF im{f.width, f.height, std::vector<float>(size_t(f.width) * f.height * 3)};
    for (size_t i = 0; i < size_t(f.width) * f.height; ++i)
        for (int k = 0; k < 3; ++k) im.rgb[i * 3 + k] = f.rgba[i * 4 + k] / 65535.f;
    return im;
}

static ImageF fromTextures(const EncodedTextures& t, int w, int h) {
    std::vector<float> rgba(size_t(t.texWidth) * t.texHeight * 4);
    decodeHapQAlphaTextures(t.ycocg.data(), t.alpha.empty() ? nullptr : t.alpha.data(), t.texWidth, t.texHeight, rgba.data());
    ImageF im{w, h, std::vector<float>(size_t(w) * h * 3)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) im.rgb[(size_t(y) * w + x) * 3 + k] = rgba[(size_t(y) * t.texWidth + x) * 4 + k];
    return im;
}

static void dump(const std::string& prefix, const ImageF& src, const ImageF& dec) {
    ImageF d8 = dec, err = dec;
    for (auto& v : d8.rgb) v = std::round(std::clamp(v, 0.f, 1.f) * 255.f) / 255.f;
    for (size_t i = 0; i < err.rgb.size(); ++i) err.rgb[i] = std::clamp(0.5f + (d8.rgb[i] - src.rgb[i]) * 16.f, 0.f, 1.f);
    writePng(prefix + "_dec.png", d8);
    writePng(prefix + "_err16x.png", err);
}

static bool loadFrame(const std::string& path, int64_t frame, Frame16& f) {
    MediaReader r;
    std::string err;
    if (!r.open(path, err)) { fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return false; }
    if (frame > 0 && !r.seekFrame(frame)) { fprintf(stderr, "seek failed\n"); return false; }
    if (!r.read(f)) { fprintf(stderr, "%s: no frame\n", path.c_str()); return false; }
    return true;
}

static bool readHapPacket(const std::string& path, int64_t frame, std::vector<uint8_t>& data, int& w, int& h) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    avformat_find_stream_info(fmt, nullptr);
    int s = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    bool ok = false;
    if (s >= 0) {
        w = fmt->streams[s]->codecpar->width;
        h = fmt->streams[s]->codecpar->height;
        AVPacket* pkt = av_packet_alloc();
        int64_t n = 0;
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == s && n++ == frame) {
                data.assign(pkt->data, pkt->data + pkt->size);
                ok = true;
                av_packet_unref(pkt);
                break;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    avformat_close_input(&fmt);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "ghap_eval gen <sky|dark|ramps|detail> <out.png> [w h]\n"
                        "ghap_eval sweep <source> [--frame N] [--dump dir]\n"
                        "ghap_eval metrics <source> <hap.mov> [--frame N] [--dump prefix]\n");
        return 2;
    }
    std::string cmd = argv[1];
    int64_t frame = 0;
    std::string dumpTo;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--frame") && i + 1 < argc) frame = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpTo = argv[++i];
    }

    if (cmd == "gen") {
        int w = argc > 5 ? atoi(argv[4]) : 1920, h = argc > 5 ? atoi(argv[5]) : 1080;
        return writePng(argv[3], generate(argv[2], w, h)) ? 0 : 1;
    }

    Frame16 srcFrame;
    if (!loadFrame(argv[2], frame, srcFrame)) return 1;
    ImageF src = fromFrame(srcFrame);

    if (cmd == "metrics") {
        std::vector<uint8_t> pkt;
        int w, h;
        if (!readHapPacket(argv[3], frame, pkt, w, h)) { fprintf(stderr, "cannot read Hap frame\n"); return 1; }
        EncodedTextures t;
        std::string err;
        if (!unpackHapFrame(pkt.data(), pkt.size(), w, h, t, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        ImageF dec = fromTextures(t, w, h);
        Metrics m = measure(src, dec);
        printf("%-40s psnr %6.2f  psnr8 %6.2f  smooth8 %6.2f  %7.1f KB\n", std::filesystem::path(argv[3]).filename().string().c_str(),
               m.psnr, m.psnr8, m.smooth8, pkt.size() / 1024.0);
        if (!dumpTo.empty()) dump(dumpTo, src, dec);
        return 0;
    }

    if (cmd == "sweep") {
        struct Variant { const char* name; Quality q; float dither; };
        const Variant variants[] = {
            {"fast d0", Quality::Fast, 0}, {"normal d0", Quality::Normal, 0}, {"high d0", Quality::High, 0},
            {"max d0", Quality::Max, 0}, {"high d0.5", Quality::High, 0.5f}, {"high d1", Quality::High, 1.0f},
        };
        FrameView view{srcFrame.rgba.data(), srcFrame.width, srcFrame.height, srcFrame.width * 4};
        for (const auto& v : variants) {
            EncoderSettings s;
            s.quality = v.q;
            s.dither = v.dither;
            EncodedTextures t;
            auto t0 = std::chrono::steady_clock::now();
            encodeTextures(view, s, 0, t);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ImageF dec = fromTextures(t, srcFrame.width, srcFrame.height);
            Metrics m = measure(src, dec);
            printf("%-12s psnr %6.2f  psnr8 %6.2f  smooth8 %6.2f  %7.1f ms\n", v.name, m.psnr, m.psnr8, m.smooth8, ms);
            if (!dumpTo.empty()) {
                std::string n = v.name;
                std::replace(n.begin(), n.end(), ' ', '_');
                dump(dumpTo + "/" + n, src, dec);
            }
        }
        return 0;
    }
    if (cmd == "try") {
        // ghap_eval try <source> c1,a4,s2,d0 ...  (colour radius, alpha radius, scale tries, dither)
        FrameView view{srcFrame.rgba.data(), srcFrame.width, srcFrame.height, srcFrame.width * 4};
        for (int i = 3; i < argc; ++i) {
            if (argv[i][0] == '-') { ++i; continue; }
            BlockSettings b;
            float d = 0;
            std::string spec = argv[i];
            for (size_t p = 0; p < spec.size();) {
                char k = spec[p];
                size_t e = spec.find(',', p);
                float v = float(atof(spec.substr(p + 1, e - p - 1).c_str()));
                if (k == 'c') b.colorRadius = int(v);
                else if (k == 'a') b.alphaRadius = int(v);
                else if (k == 's') b.scaleTries = int(v);
                else if (k == 'd') d = v;
                p = e == std::string::npos ? spec.size() : e + 1;
            }
            b.ditherChroma = b.ditherLuma = b.ditherAlpha = d;
            b.alphaWeighted = false;
            EncoderSettings s;
            s.dither = d;
            s.blockOverride = &b;
            EncodedTextures t;
            auto t0 = std::chrono::steady_clock::now();
            encodeTextures(view, s, 0, t);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            Metrics m = measure(src, fromTextures(t, srcFrame.width, srcFrame.height));
            printf("%-14s psnr %6.2f  psnr8 %6.2f  smooth8 %6.2f  %7.1f ms\n", argv[i], m.psnr, m.psnr8, m.smooth8, ms);
        }
        return 0;
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
