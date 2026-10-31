/*
 * The Extended profile's measured tables (docs/oscairn-plan.md, milestone M4).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "ext_tables.h"

#include <algorithm>
#include <cmath>

// The fitted data, written by the measurement repository's fm/fit_tables.py.
#include "ext_tables_data.h"

namespace {

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// piecewise-linear interpolation through (x[i], y[i]), x ascending; clamped at the ends
template <size_t K> double interpolate(const double (&x)[K], const double (&y)[K], double v) {
    if (v <= x[0])
        return y[0];
    for (size_t i = 1; i < K; i++)
        if (v <= x[i])
            return y[i - 1] + (y[i] - y[i - 1]) * (v - x[i - 1]) / (x[i] - x[i - 1]);
    return y[K - 1];
}

// DX7-style level scaling (Dx7Note's ScaleCurve / ScaleLevel), in dB: not measured for the
// Extended profile yet, so the Classic curves are used.
// shortcut: Classic level scaling; measure the Extended curves when level scaling is gated.
const uint8_t expScale[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47, 56, 66,
                             80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250 };
int scaleCurve(int group, int depth, int curve) {
    int scale;
    if (curve == 0 || curve == 3)
        scale = (group * depth * 329) >> 12;
    else
        scale = (expScale[std::min(group, (int)sizeof expScale - 1)] * depth * 329) >> 15;
    return curve < 2 ? -scale : scale;
}

constexpr double kDbPerStep = 32 * 6.020599913279624 / 256; // one DX7 output level step

} // namespace

double extEgMsPerDb(int t) { return kDecayMsPerDb[clampi(t, 0, 99)]; }
double extAttackMsPerDb(int t) { return kAttackMsPerDb[clampi(t, 0, 99)]; }
double extAttackOverDb() { return kAttackOverDb; }
double extAttackStartDb() { return kAttackStartDb; }
double extHoldMs(int t) { return kHoldMs[clampi(t, 0, 99)]; }

double extEgLevelDb(int l) {
    l = clampi(l, 0, 99);
    return l < kLevelSilentBelow ? -200.0 : interpolate(kLevelX, kEgLevelDb, l);
}
double extOpLevelDb(int l) {
    l = clampi(l, 0, 99);
    return l < kLevelSilentBelow ? -200.0 : interpolate(kLevelX, kOpLevelDb, l);
}

// shortcut: the Classic rate scaling curve (Dx7Note's ScaleRate) as a time factor; the
// Extended curve is not measured yet (not an M4 gate)
double extTimeKeyFollowScale(int sensitivity, int midinote) {
    const int x = std::min(31, std::max(0, midinote / 3 - 7));
    const int qrateDelta = (clampi(sensitivity, 0, 7) * x) >> 3;
    return std::pow(2.0, -qrateDelta / 4.0);
}

double extLevelScalingDb(int midinote, int breakPoint, int lowDepth, int highDepth, int lowCurve, int highCurve) {
    const int offset = midinote - breakPoint - 17;
    const int s = offset >= 0 ? scaleCurve((offset + 1) / 3, highDepth, highCurve)
                              : scaleCurve(-(offset - 1) / 3, lowDepth, lowCurve);
    return s * kDbPerStep;
}

// shortcut: Dx7Note's ScaleVelocity, mirrored for negative sensitivities (unmeasured)
double extLevelVelocityDb(int sensitivity, int velocity) {
    static const uint8_t data[64] = { 0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
                                      166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
                                      205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
                                      233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
                                      252, 253, 254 };
    const int v = data[clampi(velocity, 0, 127) >> 1] - 239;
    const int s = std::abs(clampi(sensitivity, -7, 7));
    const double db = (((s * v + 7) >> 3) << 4) / 32.0 * kDbPerStep;
    return sensitivity < 0 ? -db : db;
}

// Fine 100-127 carries into the next coarse value (fine 100 on coarse 4 is ratio 5); past
// coarse 31 the reference plays 0.5 at fine 100 and a fixed ratio above it.
double extRatio(int coarse, int fine) {
    coarse = clampi(coarse, 0, 31);
    fine = clampi(fine, 0, 127);
    if (fine >= 100) {
        coarse++;
        fine -= 100;
        if (coarse == 32)
            return fine == 0 ? kCoarseRatio[0] : kRatioPastTop;
    }
    return kCoarseRatio[coarse] * kFineFactor[fine];
}

double extDetuneCents(int detune, int midinote) {
    const int d = clampi(detune, -15, 15) + 15;
    constexpr size_t rows = sizeof kDetuneNotes / sizeof kDetuneNotes[0];
    if (midinote <= kDetuneNotes[0])
        return kDetuneCents[0][d];
    for (size_t i = 1; i < rows; i++)
        if (midinote <= kDetuneNotes[i]) {
            const double u = (midinote - kDetuneNotes[i - 1]) / (kDetuneNotes[i] - kDetuneNotes[i - 1]);
            return kDetuneCents[i - 1][d] + u * (kDetuneCents[i][d] - kDetuneCents[i - 1][d]);
        }
    return kDetuneCents[rows - 1][d];
}

double extFixedHz(int coarse, int fine) {
    return std::min(kFixedMaxHz, kFixedBaseHz * std::pow(2.0, clampi(coarse, 0, 31) - 16 + clampi(fine, 0, 127) / 128.0));
}

double extKeyFollowCents(int sensitivity, int midinote) {
    return interpolate(kKeyFollowX, kKeyFollowCentsPerKey, clampi(sensitivity, 0, 99)) * (midinote - kKeyFollowCentre);
}

// The common depth sets the offset for every operator, scaled by velocity; the operator
// sensitivity changed nothing in the reference renders (measured -7 to +7 at depths -64 and
// +63), so it is kept as a parameter and not applied.
double extPitchVelocityCents(int depth, int /*sensitivity*/, int velocity) {
    return interpolate(kPitchVelocityDepth, kPitchVelocityDepthCents, clampi(depth, -64, 63)) *
           interpolate(kPitchVelocityX, kPitchVelocityFactor, clampi(velocity, 0, 127));
}

double extPegCents(int level) { return kPegCents[clampi(level, -50, 50) + 50]; }
double extPegCentsPerMs(int t) { return kPegCentsPerMs[clampi(t, 0, 99)]; }

double extLfoHz(int speed) { return kLfoNormalHz[clampi(speed, 0, 99)]; }
double extLfoExtendedHz(int speed) { return interpolate(kLfoExtX, kLfoExtHz, clampi(speed, 0, 415)); }

int32_t extPitchDepthOffset(int o) {
    static const int32_t t[] = { 0, 10, 20, 33, 55, 92, 153, 255 }; // shortcut: DX7 pitch sensitivity (unmeasured)
    return t[clampi(o, 0, 7)];
}
int32_t extAmpDepthOffset(int o) {
    // shortcut: DX7's four amplitude sensitivities spread over 0-7 (unmeasured)
    static const int32_t t[] = { 0, 2171169, 4342338, 5756888, 7171437, 10373363, 13575290, 16777216 };
    return t[clampi(o, 0, 7)];
}
