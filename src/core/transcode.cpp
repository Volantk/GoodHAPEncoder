#include "transcode.h"

#include <chrono>
#include <condition_variable>
#include <deque>
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

} // namespace

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

    while (auto f = decoded.pop()) {
        if (cancelled()) continue; // drain so the reader can finish
        FrameView v{f->rgba.data(), f->width, f->height, f->width * 4};
        EncodedTextures tex;
        encodeTextures(v, settings, int(f->index), tex);
        encoded.push({f->index, std::move(tex)});
        p.done = f->index + 1;
        p.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        p.fps = p.elapsed > 0 ? p.done / p.elapsed : 0;
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
