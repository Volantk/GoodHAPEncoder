#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "frame_encoder.h"

namespace ghap {

// QuickTime writer for Hap frames (one video track, every frame a keyframe).
class MovWriter {
public:
    MovWriter();
    ~MovWriter();
    bool open(const std::string& path, int width, int height, int fpsNum, int fpsDen, HapFormat format, std::string& err);
    bool write(const std::vector<uint8_t>& frame, int64_t index, std::string& err);
    bool close(std::string& err); // writes the index (moov), moved to the front

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ghap
