#include "bluenoise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ghap {
namespace {

constexpr int N = kBlueNoiseSize;
constexpr int NN = N * N;

struct VoidAndCluster {
    float ranks[NN];

    std::vector<float> kernel; // toroidal gaussian by (dx, dy)
    std::vector<float> energy;
    std::vector<uint8_t> on;

    void splat(int p, float sign) {
        int px = p % N, py = p / N;
        for (int y = 0; y < N; ++y) {
            int dy = (y - py + N) % N;
            for (int x = 0; x < N; ++x) {
                int dx = (x - px + N) % N;
                energy[y * N + x] += sign * kernel[dy * N + dx];
            }
        }
    }
    int extreme(bool wantOn, bool wantMax) {
        int best = -1;
        float bv = wantMax ? -1e30f : 1e30f;
        for (int i = 0; i < NN; ++i) {
            if (bool(on[i]) != wantOn) continue;
            if (wantMax ? energy[i] > bv : energy[i] < bv) { bv = energy[i]; best = i; }
        }
        return best;
    }

    VoidAndCluster() {
        kernel.resize(NN);
        const float sigma = 1.9f;
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                int dx = std::min(x, N - x), dy = std::min(y, N - y);
                kernel[y * N + x] = std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
            }
        energy.assign(NN, 0.0f);
        on.assign(NN, 0);

        // Deterministic initial pattern, ~10% on.
        uint32_t seed = 0x9E3779B9u;
        auto rnd = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
        int initial = NN / 10;
        for (int n = 0; n < initial;) {
            int p = int(rnd() % NN);
            if (!on[p]) { on[p] = 1; splat(p, 1); ++n; }
        }
        // Relax: move the tightest cluster into the largest void until stable.
        for (int it = 0; it < NN; ++it) {
            int c = extreme(true, true);
            on[c] = 0; splat(c, -1);
            int v = extreme(false, false);
            on[v] = 1; splat(v, 1);
            if (v == c) break;
        }
        std::vector<uint8_t> initOn = on;
        std::vector<float> initEnergy = energy;

        // Phase 1: remove tightest clusters, ranks below initial.
        for (int r = initial - 1; r >= 0; --r) {
            int c = extreme(true, true);
            on[c] = 0; splat(c, -1);
            ranks[c] = float(r);
        }
        // Phase 2+3: fill largest voids.
        on = initOn;
        energy = initEnergy;
        for (int r = initial; r < NN; ++r) {
            int v = extreme(false, false);
            on[v] = 1; splat(v, 1);
            ranks[v] = float(r);
        }
        for (auto& r : ranks) r = (r + 0.5f) / NN;
    }
};

} // namespace

const float* blueNoise() {
    static VoidAndCluster vc;
    return vc.ranks;
}

float blueNoiseAt(int x, int y, int c, int frame) {
    static const int ox[4] = {0, 23, 41, 7}, oy[4] = {0, 37, 13, 53};
    const float* bn = blueNoise();
    float v = bn[((y + oy[c]) & (N - 1)) * N + ((x + ox[c]) & (N - 1))];
    v += float(frame) * 0.61803398875f;
    return v - std::floor(v) - 0.5f;
}

} // namespace ghap
