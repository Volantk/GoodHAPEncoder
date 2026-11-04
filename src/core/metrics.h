#pragma once
#include <vector>

#include "frame_encoder.h"
#include "media_reader.h"

namespace ghap {

struct ImageF {
    int w = 0, h = 0;
    std::vector<float> rgb; // interleaved RGB
};

struct Metrics {
    double psnr = 0;    // float output vs source
    double psnr8 = 0;   // output rounded to 8 bits vs source
    double smooth8 = 0; // psnr8 after a gaussian blur of both: banding, tints, blotches
};

ImageF imageFromFrame(const Frame16& f);
// Decodes like a GPU (float interpolation + Hap Q shader) and crops to w x h.
ImageF imageFromTextures(const EncodedTextures& t, int w, int h);
Metrics measure(const ImageF& src, const ImageF& dec);

// Hap Q made by FFmpeg's built-in encoder, for comparison. Source is padded
// to a multiple of 4 and reduced to 8 bits, as FFmpeg requires.
bool ffmpegHapQReference(const Frame16& src, EncodedTextures& out);

} // namespace ghap
