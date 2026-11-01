/*
 * The Extended profile's operator waves (docs/oscairn-plan.md, milestone M5).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "ext_waves.h"

#include <cmath>
#include <vector>

#include "ext_waves_data.h"

namespace extwave {
namespace {

constexpr int kSets = 32;   // waves 1-4 x spread 0-7
constexpr int kWindows = 16; // waves 5-6 x spread 0-7
constexpr int kOdd = 16;     // waves 3-4 x spread 0-7: E with the wave = E(2 t) sin(t)
constexpr double kTwoPi = 6.283185307179586;
// the versions: how many components (harmonics from 1, or window components from d = 0) each holds
constexpr int kCounts[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 24, 28, 32, 40, 48,
                            56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512 };
constexpr int kVersions = sizeof kCounts / sizeof kCounts[0];

// tables of kSize + 1 samples (the last repeats the first, for interpolation); versions that
// add nothing to the one before share its table
std::vector<int32_t> &storage() {
    static std::vector<int32_t> s;
    return s;
}
size_t version[kSets + kWindows + kOdd][kVersions]; // offsets into storage()

void build(int set, const float *amp, const float *phase, int count, int first, double phaseStep, double dc,
           std::vector<int32_t> &out, bool odd = false) {
    std::vector<double> acc(kSize, dc);
    int done = 0;
    for (int v = 0; v < kVersions; v++) {
        const int top = kCounts[v];
        if (v > 0 && done >= count) { // nothing left to add
            version[set][v] = version[set][v - 1];
            continue;
        }
        for (int k = done; k < top && k < count; k++) {
            if (amp[k] == 0.0f)
                continue;
            const int h = k + first; // harmonic number (or window component d)
            const double ph = phase[k] + phaseStep * h;
            if (odd) {
                if (h % 2 == 0) // (an odd wave has none)
                    continue;
                // harmonic h = 2K + 1, C cos(h t) + S sin(h t), as E(2 t) sin(t): E gains S times the
                // Dirichlet kernel 1 + 2 sum cos(k x) and -2 C times sum sin(k x), k = 1..K
                const double c = amp[k] * std::cos(ph), sn = -amp[k] * std::sin(ph);
                const int kk = (h - 1) / 2;
                acc[0] += sn * (2 * kk + 1);
                for (int i = 1; i < kSize; i++) {
                    const double x = kTwoPi * i / kSize, half = std::sin(x / 2);
                    acc[i] += sn * std::sin((kk + 0.5) * x) / half - c * (std::cos(x / 2) - std::cos((kk + 0.5) * x)) / half;
                }
                continue;
            }
            for (int i = 0; i < kSize; i++)
                acc[i] += amp[k] * std::cos(kTwoPi * h * i / kSize + ph);
        }
        done = top;
        version[set][v] = out.size();
        for (int i = 0; i < kSize; i++)
            out.push_back((int32_t)std::lround(acc[i] * (1 << 24)));
        out.push_back(out[version[set][v]]);
    }
}

// the largest version holding no more than n components (at least the first version)
int versionFor(int n) {
    int v = 0;
    while (v + 1 < kVersions && kCounts[v + 1] <= n)
        v++;
    return v;
}

} // namespace

void init() {
    static const bool ready = [] {
        std::vector<int32_t> &out = storage();
        // waves 1-4: cos(h t + phase_h - h pi / 2) plus a constant (waves 1 and 2 run from 0
        // to their peak); the phases place the wave in its cycle as the reference does: waves 1
        // and 2 start half a cycle on, at their minimum (`m9a`: as modulators of a sine `spec5`
        // could not tell the two apart)
        for (int s = 0; s < kSets; s++)
            build(s, kWaveHarmonicAmp + kWaveHarmonicOffset[s], kWaveHarmonicPhase + kWaveHarmonicOffset[s],
                  kWaveHarmonicCount[s], 1, s < 16 ? kTwoPi / 4 : -kTwoPi / 4, kWaveHarmonicDc[s], out);
        // waves 3-4 again as E, the wave being E(2 t) sin(t) (their constant is 0)
        for (int s = 16; s < kSets; s++)
            build(kSets + kWindows + s - 16, kWaveHarmonicAmp + kWaveHarmonicOffset[s],
                  kWaveHarmonicPhase + kWaveHarmonicOffset[s], kWaveHarmonicCount[s], 1, -kTwoPi / 4, 0.0, out, true);
        // windows: component d from 0
        for (int s = 0; s < kWindows; s++)
            build(kSets + s, kWaveWindowAmp + kWaveWindowOffset[s], kWaveWindowPhase + kWaveWindowOffset[s],
                  kWaveWindowCount[s], 0, 0.0, 0.0, out);
        out.shrink_to_fit();
        return true;
    }();
    (void)ready;
}

const int32_t *table(int wave, int spread, int maxHarmonic) {
    return storage().data() + version[(wave - 1) * 8 + spread][versionFor(maxHarmonic)];
}

const int32_t *oddWindow(int wave, int spread, int maxHarmonic) {
    return storage().data() + version[kSets + kWindows + (wave - 3) * 8 + spread][versionFor(maxHarmonic)];
}

const int32_t *window(int wave, int spread, int maxComponent) {
    if (maxComponent < 0)
        return nullptr;
    return storage().data() + version[kSets + (wave - 5) * 8 + spread][versionFor(maxComponent + 1)];
}

} // namespace extwave
