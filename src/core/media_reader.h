#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ghap {

struct MediaInfo {
    int width = 0, height = 0;
    int fpsNum = 25, fpsDen = 1;
    int64_t frameCount = 0; // estimate, 0 if unknown
    double duration = 0;    // seconds
    bool hasAlpha = false;
    int bitDepth = 8;
    std::string codec, pixelFormat, colorInfo;
    bool isImageSequence = false;
};

// RGBA, 16 bits per channel, tightly packed (stride = width * 4).
struct Frame16 {
    int width = 0, height = 0;
    int64_t index = 0;
    std::vector<uint16_t> rgba;
};

class MediaReader {
public:
    MediaReader();
    ~MediaReader();
    MediaReader(const MediaReader&) = delete;
    MediaReader& operator=(const MediaReader&) = delete;

    // path may be a video file, a still image, or an image sequence pattern (name_%04d.png).
    bool open(const std::string& path, std::string& err);
    const MediaInfo& info() const;

    // Next frame in decode order; false at end of stream or on error.
    bool read(Frame16& out);
    // Seek to a frame index (nearest at or before, then decode forward).
    bool seekFrame(int64_t index);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// If path is one file of a numbered sequence (shot_0001.png), returns the
// ffmpeg pattern and first number; otherwise returns false.
bool detectImageSequence(const std::string& path, std::string& pattern, int& firstNumber, int& count);

} // namespace ghap
