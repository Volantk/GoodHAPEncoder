#include "media_reader.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <regex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

namespace ghap {

struct MediaReader::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgb = nullptr;
    int stream = -1;
    bool eof = false;
    int64_t nextIndex = 0;
    int64_t seekTargetPts = AV_NOPTS_VALUE;
    bool pending = false; // frame holds a decoded frame not yet returned
    MediaInfo info;

    ~Impl() {
        av_frame_free(&rgb);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        sws_free_context(&sws);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }

    // Returns 1 with a frame, 0 at end, <0 on error.
    int decodeOne() {
        for (;;) {
            int r = avcodec_receive_frame(dec, frame);
            if (r == 0) return 1;
            if (r == AVERROR_EOF) return 0;
            if (r != AVERROR(EAGAIN)) return r;
            if (eof) return 0;
            r = av_read_frame(fmt, pkt);
            if (r < 0) {
                eof = true;
                avcodec_send_packet(dec, nullptr);
                continue;
            }
            if (pkt->stream_index == stream) avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
        }
    }

    bool convert(Frame16& out) {
        // Untagged HD video is almost always BT.709; swscale would assume 601.
        if (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720) frame->colorspace = AVCOL_SPC_BT709;
        av_frame_unref(rgb);
        rgb->format = AV_PIX_FMT_RGBA64LE;
        rgb->width = frame->width;
        rgb->height = frame->height;
        // Keep primaries and transfer: we only want the matrix and range undone,
        // never a gamut or transfer conversion.
        rgb->color_primaries = frame->color_primaries;
        rgb->color_trc = frame->color_trc;
        rgb->colorspace = AVCOL_SPC_RGB;
        rgb->color_range = AVCOL_RANGE_JPEG;
        if (sws_scale_frame(sws, rgb, frame) < 0) return false;
        out.width = rgb->width;
        out.height = rgb->height;
        out.rgba.resize(size_t(out.width) * out.height * 4);
        for (int y = 0; y < out.height; ++y)
            std::memcpy(out.rgba.data() + size_t(y) * out.width * 4, rgb->data[0] + size_t(y) * rgb->linesize[0],
                        size_t(out.width) * 8);
        return true;
    }
};

MediaReader::MediaReader() : d(new Impl) {}
MediaReader::~MediaReader() = default;
const MediaInfo& MediaReader::info() const { return d->info; }

static std::string errStr(int e) {
    char buf[256];
    av_strerror(e, buf, sizeof buf);
    return buf;
}

bool MediaReader::open(const std::string& pathIn, std::string& err) {
    std::string path = pathIn;
    AVDictionary* opts = nullptr;
    const AVInputFormat* forced = nullptr;
    std::string pattern;
    int first = 0, count = 0;
    bool seq = false;
    if (detectImageSequence(pathIn, pattern, first, count) && count > 1) {
        path = pattern;
        seq = true;
        forced = av_find_input_format("image2");
        av_dict_set_int(&opts, "start_number", first, 0);
        av_dict_set(&opts, "framerate", "30", 0);
    }
    int r = avformat_open_input(&d->fmt, path.c_str(), forced, &opts);
    av_dict_free(&opts);
    if (r < 0) { err = "open failed: " + errStr(r); return false; }
    if ((r = avformat_find_stream_info(d->fmt, nullptr)) < 0) { err = "no stream info: " + errStr(r); return false; }
    d->stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->stream < 0) { err = "no video stream"; return false; }
    AVStream* st = d->fmt->streams[d->stream];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { err = "no decoder for this codec"; return false; }
    d->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d->dec, st->codecpar);
    d->dec->thread_count = 0;
    d->dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if ((r = avcodec_open2(d->dec, codec, nullptr)) < 0) { err = "decoder open failed: " + errStr(r); return false; }

    d->sws = sws_alloc_context();
    d->sws->flags = (SwsFlags)(SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP);
    d->sws->dither = SWS_DITHER_NONE;
    d->sws->threads = 0;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->rgb = av_frame_alloc();

    MediaInfo& mi = d->info;
    mi.width = st->codecpar->width;
    mi.height = st->codecpar->height;
    AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (!fr.num || !fr.den) fr = {25, 1};
    mi.fpsNum = fr.num;
    mi.fpsDen = fr.den;
    mi.isImageSequence = seq;
    if (st->nb_frames > 0) mi.frameCount = st->nb_frames;
    else if (seq) mi.frameCount = count;
    else if (d->fmt->duration > 0)
        mi.frameCount = av_rescale(d->fmt->duration, fr.num, int64_t(fr.den) * AV_TIME_BASE);
    if (d->fmt->duration > 0) mi.duration = d->fmt->duration / double(AV_TIME_BASE);
    else if (mi.frameCount) mi.duration = mi.frameCount * double(fr.den) / fr.num;
    mi.codec = codec->name;
    auto pf = AVPixelFormat(st->codecpar->format);
    if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pf)) {
        mi.pixelFormat = desc->name;
        mi.hasAlpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
        mi.bitDepth = desc->comp[0].depth;
    }
    // Headers can disagree with the decoded frames (DNxHR pads to 1904 lines),
    // so decode the first frame now and trust its size.
    if (d->decodeOne() == 1) {
        d->pending = true;
        mi.width = d->frame->width;
        mi.height = d->frame->height;
    }
    mi.colorInfo = std::string(av_color_space_name(st->codecpar->color_space) ? av_color_space_name(st->codecpar->color_space) : "?") +
                   "/" + (av_color_primaries_name(st->codecpar->color_primaries) ? av_color_primaries_name(st->codecpar->color_primaries) : "?") +
                   "/" + (av_color_transfer_name(st->codecpar->color_trc) ? av_color_transfer_name(st->codecpar->color_trc) : "?");
    return true;
}

bool MediaReader::read(Frame16& out) {
    for (;;) {
        if (d->pending) d->pending = false;
        else if (d->decodeOne() <= 0) return false;
        if (d->seekTargetPts != AV_NOPTS_VALUE) {
            int64_t pts = d->frame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE && pts < d->seekTargetPts) { av_frame_unref(d->frame); continue; }
            d->seekTargetPts = AV_NOPTS_VALUE;
        }
        bool ok = d->convert(out);
        av_frame_unref(d->frame);
        if (!ok) return false;
        out.index = d->nextIndex++;
        return true;
    }
}

bool MediaReader::seekFrame(int64_t index) {
    AVStream* st = d->fmt->streams[d->stream];
    AVRational frameDur = {d->info.fpsDen, d->info.fpsNum};
    int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    int64_t target = start + av_rescale_q(index, frameDur, st->time_base);
    // Half a frame of slack for rounding in the timestamps.
    int64_t half = av_rescale_q(1, frameDur, st->time_base) / 2;
    if (av_seek_frame(d->fmt, d->stream, target, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(d->dec);
    av_frame_unref(d->frame);
    d->pending = false;
    d->eof = false;
    d->seekTargetPts = target - half;
    d->nextIndex = index;
    return true;
}

static fs::path toPath(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}
static std::string fromPath(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool detectImageSequence(const std::string& path, std::string& pattern, int& firstNumber, int& count) {
    fs::path p = toPath(path);
    std::string name = fromPath(p.filename());
    static const std::regex re(R"(^(.*?)(\d+)(\.[A-Za-z0-9]+)$)");
    std::smatch m;
    if (!std::regex_match(name, m, re)) return false;
    std::string ext = m[3].str();
    std::string lower = ext;
    for (auto& c : lower) c = char(tolower(c));
    static const char* stills[] = {".png", ".tif", ".tiff", ".exr", ".dpx", ".jpg", ".jpeg", ".tga", ".bmp", ".webp"};
    if (std::none_of(std::begin(stills), std::end(stills), [&](const char* s) { return lower == s; })) return false;
    std::string prefix = m[1].str();
    size_t digits = m[2].str().size();
    int lo = INT32_MAX, n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(p.parent_path().empty() ? fs::path(".") : p.parent_path(), ec)) {
        std::string f = fromPath(e.path().filename());
        std::smatch fm;
        if (!std::regex_match(f, fm, re)) continue;
        if (fm[1].str() != prefix || fm[3].str() != ext || fm[2].str().size() != digits) continue;
        lo = std::min(lo, std::stoi(fm[2].str()));
        ++n;
    }
    if (n == 0) return false;
    std::string esc;
    for (char c : prefix) { esc += c; if (c == '%') esc += '%'; }
    char fmtbuf[16];
    snprintf(fmtbuf, sizeof fmtbuf, "%%0%zud", digits);
    pattern = fromPath(p.parent_path() / toPath(esc + fmtbuf + ext));
    firstNumber = lo;
    count = n;
    return true;
}

} // namespace ghap
