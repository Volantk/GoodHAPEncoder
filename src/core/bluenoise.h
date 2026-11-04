#pragma once

namespace ghap {

constexpr int kBlueNoiseSize = 64;

// 64x64 blue noise ranks in [0, 1), made once with void-and-cluster.
const float* blueNoise();

// Noise in [-0.5, 0.5) for pixel (x, y), channel c (0..3), frame f.
// Channels use offset tiles; frames advance by the golden ratio so the
// pattern changes every frame but each frame stays blue.
float blueNoiseAt(int x, int y, int c, int frame);

} // namespace ghap
