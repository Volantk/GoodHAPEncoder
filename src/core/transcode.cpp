#include "transcode.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "media_reader.h"
#include "mov_writer.h"

namespace ghap {
namespace {

template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t cap) : cap_(cap) {}
    bool push(T v) {
        std::unique_lock l(m_);
        notFull_.wait(l, [&] { return q_.size() < cap_ || closed_; });
        if (closed_) return false;
        q_.push_back(std::move(v));
        notEmpty_.notify_one();
        return true;
    }
    std::optional<T> pop() {
        std::unique_lock l(m_);
        notEmpty_.wait(l, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return v;
    }
    void close() {
        std::lock_guard l(m_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<T> q_;
    size_t cap_;
    bool closed_ = false;
};

std::filesystem::path toPath(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}
std::string fromPath(const std::filesystem::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

std::string defaultOutputPath(const std::string& input, bool isSequence, HapFormat fmt, const std::string& dir) {
    namespace fs = std::filesystem;
    fs::path src = toPath(input);
    std::string stem = fromPath(src.stem());
    if (isSequence) {
        while (!stem.empty() && isdigit((unsigned char)stem.back())) stem.pop_back();
        while (!stem.empty() && (stem.back() == '_' || stem.back() == '.' || stem.back() == '-' || stem.back() == ' ')) stem.pop_back();
        if (stem.empty()) stem = fromPath(src.parent_path().filename());
    }
    fs::path outDir = dir.empty() ? src.parent_path() : toPath(dir);
    std::string suffix = fmt == HapFormat::HapQAlpha ? "_HapQAlpha" : "_HapQ";
    fs::path out = outDir / toPath(stem + suffix + ".mov");
    std::error_code ec;
    for (int n = 2; fs::exists(out, ec); ++n) out = outDir / toPath(stem + suffix + " (" + std::to_string(n) + ").mov");
    return fromPath(out);
}

bool transcode(const TranscodeJob& job, const std::function<void(const TranscodeProgress&)>& progress,
               const std::atomic<bool>* cancel, std::string& err) {
    MediaReader reader;
    if (!reader.open(job.input, err)) return false;
    const MediaInfo& mi = reader.info();
    EncoderSettings settings = job.settings;
    if (job.autoAlpha && mi.hasAlpha) settings.format = HapFormat::HapQAlpha;

    MovWriter writer;
    if (!writer.open(job.output, mi.width, mi.height, mi.fpsNum, mi.fpsDen, settings.format, err)) return false;

    BoundedQueue<Frame16> decoded(3);
    BoundedQueue<std::pair<int64_t, EncodedTextures>> encoded(2);
    std::atomic<bool> failed{false};
    std::string writeErr;
    auto cancelled = [&] { return (cancel && cancel->load()) || failed.load(); };

    std::thread readThread([&] {
        int64_t n = 0;
        for (;;) {
            if (cancelled() || (job.maxFrames && n >= job.maxFrames)) break;
            Frame16 f;
            if (!reader.read(f)) break;
            if (!decoded.push(std::move(f))) break;
            ++n;
        }
        decoded.close();
    });

    TranscodeProgress p;
    p.total = job.maxFrames ? std::min<int64_t>(job.maxFrames, mi.frameCount ? mi.frameCount : job.maxFrames) : mi.frameCount;
    auto t0 = std::chrono::steady_clock::now();
    std::atomic<int64_t> written{0};

    std::thread writeThread([&] {
        std::vector<uint8_t> frame;
        while (auto item = encoded.pop()) {
            if (failed) continue;
            if (!packHapFrame(item->second, settings, frame)) { writeErr = "Hap frame packing failed"; failed = true; continue; }
            if (!writer.write(frame, item->first, writeErr)) { failed = true; continue; }
            written++;
        }
    });

    std::deque<double> stamps; // finish times of recent frames, for recentFps
    while (auto f = decoded.pop()) {
        if (cancelled()) continue; // drain so the reader can finish
        FrameView v{f->rgba.data(), f->width, f->height, f->width * 4};
        EncodedTextures tex;
        auto e0 = std::chrono::steady_clock::now();
        encodeTextures(v, settings, int(f->index), tex);
        auto e1 = std::chrono::steady_clock::now();
        encoded.push({f->index, std::move(tex)});
        p.done = f->index + 1;
        p.elapsed = std::chrono::duration<double>(e1 - t0).count();
        p.fps = p.elapsed > 0 ? p.done / p.elapsed : 0;
        p.frameMs = std::chrono::duration<double, std::milli>(e1 - e0).count();
        stamps.push_back(p.elapsed);
        while (stamps.size() > 2 && stamps.back() - stamps.front() > 2.0) stamps.pop_front();
        p.recentFps = stamps.size() > 1 ? (stamps.size() - 1) / std::max(1e-6, stamps.back() - stamps.front()) : p.fps;
        if (progress) progress(p);
    }
    encoded.close();
    readThread.join();
    writeThread.join();

    std::string closeErr;
    bool closed = writer.close(closeErr);
    if (failed) { err = writeErr; return false; }
    if (cancel && cancel->load()) { err = "cancelled"; return false; }
    if (!closed) { err = closeErr; return false; }
    if (written == 0) { err = "no frames decoded"; return false; }
    return true;
}

} // namespace ghap
