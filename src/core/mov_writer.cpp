#include "mov_writer.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace ghap {

struct MovWriter::Impl {
    AVFormatContext* oc = nullptr;
    AVStream* st = nullptr;
    AVRational frameDur{1, 25};
    bool headerWritten = false;

    ~Impl() {
        if (oc) {
            if (oc->pb) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    }
};

MovWriter::MovWriter() : d(new Impl) {}
MovWriter::~MovWriter() = default;

static std::string errStr(int e) {
    char buf[256];
    av_strerror(e, buf, sizeof buf);
    return buf;
}

bool MovWriter::open(const std::string& path, int width, int height, int fpsNum, int fpsDen, HapFormat format, std::string& err) {
    int r = avformat_alloc_output_context2(&d->oc, nullptr, "mov", path.c_str());
    if (r < 0) { err = errStr(r); return false; }
    d->st = avformat_new_stream(d->oc, nullptr);
    AVCodecParameters* cp = d->st->codecpar;
    cp->codec_type = AVMEDIA_TYPE_VIDEO;
    cp->codec_id = AV_CODEC_ID_HAP;
    const char* cc = formatFourCC(format);
    cp->codec_tag = MKTAG(cc[0], cc[1], cc[2], cc[3]);
    cp->width = width;
    cp->height = height;
    cp->bits_per_coded_sample = format == HapFormat::HapQAlpha ? 32 : 24;
    cp->color_range = AVCOL_RANGE_JPEG;
    d->frameDur = {fpsDen, fpsNum};
    d->st->time_base = d->frameDur;
    d->st->avg_frame_rate = {fpsNum, fpsDen};
    av_dict_set(&d->st->metadata, "encoder", formatName(format), 0);
    av_dict_set(&d->oc->metadata, "encoder", "GoodHAP", 0);

    if ((r = avio_open(&d->oc->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0) { err = "cannot write " + path + ": " + errStr(r); return false; }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    r = avformat_write_header(d->oc, &opts);
    av_dict_free(&opts);
    if (r < 0) { err = "header: " + errStr(r); return false; }
    d->headerWritten = true;
    return true;
}

bool MovWriter::write(const std::vector<uint8_t>& frame, int64_t index, std::string& err) {
    AVPacket* pkt = av_packet_alloc();
    if (av_new_packet(pkt, int(frame.size())) < 0) { av_packet_free(&pkt); err = "out of memory"; return false; }
    memcpy(pkt->data, frame.data(), frame.size());
    pkt->stream_index = d->st->index;
    pkt->pts = pkt->dts = av_rescale_q(index, d->frameDur, d->st->time_base);
    pkt->duration = av_rescale_q(1, d->frameDur, d->st->time_base);
    pkt->flags |= AV_PKT_FLAG_KEY;
    int r = av_write_frame(d->oc, pkt);
    av_packet_free(&pkt);
    if (r < 0) { err = "write: " + errStr(r); return false; }
    return true;
}

bool MovWriter::close(std::string& err) {
    if (!d->headerWritten) return true;
    d->headerWritten = false;
    int r = av_write_trailer(d->oc);
    avio_closep(&d->oc->pb);
    if (r < 0) { err = "trailer: " + errStr(r); return false; }
    return true;
}

} // namespace ghap
