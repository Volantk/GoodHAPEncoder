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
    double fps = 0, elapsed = 0;
};

bool transcode(const TranscodeJob& job, const std::function<void(const TranscodeProgress&)>& progress,
               const std::atomic<bool>* cancel, std::string& err);

} // namespace ghap
