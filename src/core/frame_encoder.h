#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "block_encode.h"

namespace ghap {

enum class HapFormat { HapQ, HapQAlpha };

enum class Quality { Fast, Normal, High, Max };

struct EncoderSettings {
    HapFormat format = HapFormat::HapQ;
    Quality quality = Quality::High;
    float dither = 1.0f;       // master dither strength, 0 = off
    bool animateDither = true; // new noise pattern every frame
    // Smaller files for some quality, 0 (off) to 4. Each step triples how much
    // error a saved byte may cost; smooth gradients are protected at every level.
    float compact = 0.0f;
    int chunks = 0;            // 0 = auto
    bool snappy = true;
    const BlockSettings* blockOverride = nullptr; // experiments: replaces the quality preset
};

const char* formatName(HapFormat f);
const char* formatFourCC(HapFormat f);
BlockSettings blockSettingsFor(const EncoderSettings& s);

// Source frame: RGBA, 16 bits per channel, rows of `stride` uint16 values.
struct FrameView {
    const uint16_t* rgba = nullptr;
    int width = 0, height = 0;
    int stride = 0; // in uint16 units
};

struct EncodedTextures {
    int texWidth = 0, texHeight = 0; // padded to multiples of 4
    std::vector<uint8_t> ycocg;      // BC3 blocks, Hap Q
    std::vector<uint8_t> alpha;      // BC4 blocks, Hap Q Alpha only
};

// Compress the textures (multithreaded over block rows).
void encodeTextures(const FrameView& src, const EncoderSettings& s, int frameIndex, EncodedTextures& out);

// Wrap textures into a Hap frame (sections, chunking, snappy). Returns false on failure.
bool packHapFrame(const EncodedTextures& tex, const EncoderSettings& s, std::vector<uint8_t>& frame);

// Unpack a Hap Q / Hap Q Alpha frame back into textures.
bool unpackHapFrame(const uint8_t* data, size_t size, int width, int height, EncodedTextures& out, std::string* err = nullptr);

} // namespace ghap
