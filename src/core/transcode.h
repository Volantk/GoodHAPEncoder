#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "frame_encoder.h"

namespace ghap {

struct TranscodeJob {
    std::string input, output;
    EncoderSettings settings;
    bool autoAlpha = true; // switch to Hap Q Alpha when the source has alpha
    int64_t maxFrames = 0; // 0 = all
};

struct TranscodeProgress {
    int64_t done = 0, total = 0;
    double fps = 0, elapsed = 0; // fps: average since the start
    double recentFps = 0;        // over the last ~2 seconds
    double frameMs = 0;          // encode time of the latest frame (block search only)
};

// <dir>/<name>_HapQ.mov (or _HapQAlpha), next to the source when dir is empty.
// Image sequences drop the frame number (shot_0001.png -> shot_HapQ.mov).
// Never returns an existing file: adds " (2)", " (3)" ...
std::string defaultOutputPath(const std::string& input, bool isSequence, HapFormat fmt, const std::string& dir = {});

bool transcode(const TranscodeJob& job, const std::function<void(const TranscodeProgress&)>& progress,
               const std::atomic<bool>* cancel, std::string& err);

} // namespace ghap
