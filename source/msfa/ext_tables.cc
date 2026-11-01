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
double extModLevelDb(int l) {
    l = clampi(l, 0, 99);
    return l < kModLevelX[0] ? -200.0 : interpolate(kModLevelX, kModLevelDb, l);
}
// a modulator's EG level steps as its operator level does, in steps of two: an even level plays as
// the odd one above it (measured at every third level with operator levels 99 and 70: the two add
// as values, not on the carrier's EG curve)
double extModEgLevelDb(int l) {
    l = clampi(l, 0, 99) | 1;
    return l < kModLevelX[0] ? -200.0 : interpolate(kModLevelX, kModLevelDb, l) - kModLevelDb[31];
}
double extFeedbackScale(int f) { return kFeedbackScale[clampi(f, 0, 7)]; }
double extAlgorithmGainDb(int a) { return kAlgorithmGainDb[clampi(a, 1, 88) - 1]; }

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

// Operator level velocity sensitivity -7..+7: dB against sensitivity 0, measured on a lone
// carrier at every sensitivity and 17 velocities (fifth session, `m7d`), straight between them.
// Positive values raise the level at high velocities too (+5.3 dB at +7, velocity 127); negative
// ones mirror them about velocity 64; -7 at velocity 127 is silent.
static const float kLevelVelocityDb[15][17] = {
    { 5.27f, 4.14f, 2.63f, 1.13f, -0.38f, -2.63f, -4.89f, -7.53f, -10.54f, -13.18f, -16.19f, -19.96f, -23.73f, -29.02f, -35.84f, -45.05f, -120.0f }, // -7 (127: silent)
    { 4.52f, 3.39f, 2.26f, 1.13f, -0.38f, -2.26f, -4.14f, -6.4f, -9.03f, -11.29f, -13.93f, -16.94f, -20.33f, -24.86f, -30.53f, -38.5f, -70.68f }, // -6
    { 3.76f, 3.01f, 1.88f, 0.75f, -0.38f, -1.88f, -3.39f, -5.27f, -7.53f, -9.41f, -11.67f, -14.31f, -16.94f, -20.71f, -25.62f, -32.04f, -57.07f }, // -5
    { 3.01f, 2.26f, 1.51f, 0.75f, -0.38f, -1.51f, -2.63f, -4.14f, -6.02f, -7.53f, -9.41f, -11.29f, -13.55f, -16.57f, -20.33f, -25.62f, -45.05f }, // -4
    { 2.26f, 1.88f, 1.13f, 0.75f, 0.0f, -1.13f, -1.88f, -3.01f, -4.52f, -5.65f, -6.77f, -8.28f, -10.16f, -12.42f, -15.06f, -19.2f, -33.56f }, // -3
    { 1.51f, 1.13f, 0.75f, 0.38f, 0.0f, -0.75f, -1.13f, -1.88f, -3.01f, -3.76f, -4.52f, -5.65f, -6.77f, -8.28f, -10.16f, -12.8f, -22.22f }, // -2
    { 0.75f, 0.75f, 0.38f, 0.38f, 0.0f, -0.38f, -0.38f, -0.75f, -1.51f, -1.88f, -2.26f, -2.63f, -3.39f, -4.14f, -4.89f, -6.4f, -10.92f }, // -1
    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, // +0
    { -8.28f, -6.02f, -4.89f, -3.76f, -3.39f, -2.63f, -2.26f, -1.88f, -1.51f, -0.75f, -0.38f, -0.38f, 0.0f, 0.38f, 0.38f, 0.76f, 0.68f }, // +1
    { -16.57f, -12.42f, -9.79f, -7.9f, -6.77f, -5.65f, -4.52f, -3.76f, -3.01f, -1.88f, -1.13f, -0.75f, 0.0f, 0.38f, 0.75f, 1.51f, 1.51f }, // +2
    { -24.86f, -18.45f, -15.06f, -12.05f, -10.16f, -8.28f, -6.77f, -5.65f, -4.52f, -3.01f, -1.88f, -1.13f, 0.0f, 0.75f, 1.13f, 1.88f, 2.26f }, // +3
    { -33.18f, -24.86f, -19.96f, -16.19f, -13.55f, -11.29f, -9.41f, -7.53f, -6.02f, -4.14f, -2.63f, -1.51f, -0.38f, 0.75f, 1.51f, 2.63f, 3.01f }, // +4
    { -41.57f, -31.29f, -24.86f, -20.33f, -16.94f, -14.31f, -11.67f, -9.41f, -7.53f, -5.27f, -3.39f, -1.88f, -0.38f, 1.13f, 1.88f, 3.39f, 3.76f }, // +5
    { -50.18f, -37.36f, -30.15f, -24.48f, -20.33f, -16.94f, -13.93f, -11.29f, -9.03f, -6.4f, -4.14f, -2.26f, -0.38f, 1.13f, 2.26f, 3.76f, 4.52f }, // +6
    { -59.11f, -43.88f, -35.09f, -28.64f, -23.73f, -19.96f, -16.19f, -13.18f, -10.54f, -7.53f, -4.89f, -2.63f, -0.38f, 1.51f, 2.63f, 4.52f, 5.27f }, // +7
};
static const uint8_t kLevelVelocities[17] = { 1, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127 };
// Between velocities 1 and 8, and below 1 (the part's velocity sensitivity can leave a fraction),
// positive sensitivities follow +7's finer curve in proportion (`m7f`: part velocities in steps of
// 1/64 to 1/8 at +7, the levels against velocity 1; at 1/4 and below +7 is silent).
static const float kLowVelocities[] = { 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1, 2, 3, 4, 5, 6, 8 };
static const float kLowVelocityDb[] = { -60.0f, -16.26f, -12.25f, -10.96f, -6.08f, -2.88f, 0, 2.98f, 5.85f, 8.62f, 10.96f, 12.13f, 15.23f };
double extLevelVelocityDb(int sensitivity, double velocity) {
    const float *row = kLevelVelocityDb[clampi(sensitivity, -7, 7) + 7];
    if (sensitivity > 0 && velocity < 8) {
        const double v = std::max(velocity, 0.25);
        int i = 0;
        while (kLowVelocities[i + 1] < v)
            i++;
        const double t = (v - kLowVelocities[i]) / (kLowVelocities[i + 1] - kLowVelocities[i]);
        const double db = kLowVelocityDb[i] + (kLowVelocityDb[i + 1] - kLowVelocityDb[i]) * t;
        return row[0] + db * std::min(sensitivity, 7) / 7.0;
    }
    const double v = std::clamp(velocity, 1.0, 127.0); // (shortcut: below 1 unmeasured for sensitivities up to 0)
    int i = 0;
    while (i < 15 && kLevelVelocities[i + 1] <= v)
        i++;
    if (v >= 127)
        return row[16];
    const double t = (v - kLevelVelocities[i]) / (kLevelVelocities[i + 1] - kLevelVelocities[i]);
    return row[i] + (row[i + 1] - row[i]) * t;
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
    // measured at depths 30 and 99 (the DX7's sensitivities at 1-3; 4-7 are deeper)
    static const int32_t t[] = { 0, 10, 20, 33, 56, 96, 160, 256 };
    return t[clampi(o, 0, 7)];
}
int32_t extLfoPitchDepthSteps(int depth) {
    // the DX7's depth steps, but 99 plays as 256 (measured at 17 depths)
    return depth >= 99 ? 256 : (std::max(depth, 0) * 165) >> 6;
}
double extLfoPitchCents(int depth, int offset) {
    // twice the DX7's depth: 256 x 256 steps swing 2400 cents
    return extLfoPitchDepthSteps(depth) * extPitchDepthOffset(offset) * 1200.0 / 32768;
}
// the 2nd LFO's amplitude depth: steps of 0.375 dB lost at the LFO's bottom with offset 7, straight
// in dB from its top (measured at 26 depths; straight between them)
static const double kLfoAmpX[] = { 0, 2, 3, 4, 6, 8, 10, 12, 16, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 99 };
static const double kLfoAmpSteps[] = { 0, 0, 1, 1, 2, 3, 4, 5, 8, 10, 13, 16, 20, 24, 28, 32, 37, 42, 49, 56, 65, 75, 89, 111, 149, 256 };
double extLfoAmpDb(int depth) { return 0.375 * interpolate(kLfoAmpX, kLfoAmpSteps, clampi(depth, 0, 99)); }
double extAmpDepthShare(int o) {
    // measured at depths 16, 30, 70 and 99: eighths
    static const double t[] = { 0, 1, 2, 4, 5, 6, 7, 8 };
    return t[clampi(o, 0, 7)] / 8;
}
int32_t extAmpDepthOffset(int o) {
    // shortcut: DX7's four amplitude sensitivities spread over 0-7 (unmeasured; the mod wheel only)
    static const int32_t t[] = { 0, 2171169, 4342338, 5756888, 7171437, 10373363, 13575290, 16777216 };
    return t[clampi(o, 0, 7)];
}

// ---- filter (milestone M6) ----

#include "ext_filter_data.h"

double extFilterCutoffHz(double value) {
    value = std::clamp(value, 0.0, 1023.0);
    const int i = std::min((int)value, 1022);
    return kFilterCutoffHz[i] + (kFilterCutoffHz[i + 1] - kFilterCutoffHz[i]) * (value - i);
}

double extFilterMaxHz() { return kFilterMaxHz; }

double extFilterMaxDamping(double fc) {
    constexpr size_t K = sizeof(kFilterMinQ) / sizeof(kFilterMinQ[0]);
    double q = 0.5;
    if (fc >= kFilterMinQHz[K - 1])
        q = kFilterMinQ[K - 1];
    else
        for (size_t i = 1; i < K; i++)
            if (fc < kFilterMinQHz[i]) {
                if (fc >= kFilterMinQHz[i - 1])
                    q = kFilterMinQ[i - 1] + (kFilterMinQ[i] - kFilterMinQ[i - 1]) * (fc - kFilterMinQHz[i - 1]) /
                                                 (kFilterMinQHz[i] - kFilterMinQHz[i - 1]);
                break;
            }
    return 1 / std::max(q, 0.5);
}

double extFilterQ(int resonance) { return 0.5 * std::exp2(std::clamp(resonance, 0, 127) / 16.0); }

namespace {
// a frequency ratio for a move of u octaves, straight in frequency within each octave (exact
// at whole octaves): how the reference applies key follow and upward velocity moves
double linearOctaves(double u) {
    const double e = std::floor(u);
    return std::exp2(e) * (1 + (u - e));
}
} // namespace

double extFilterKeyFollowRatio(int sensitivity, int midinote) {
    // -64-+63 in steps of 3.125 %, +63 playing +200 % (measured at 32, 54, 74, 96 and 127
    // raw); centre note 60
    const int s = std::clamp(sensitivity, -64, 63);
    const double percent = s == 63 ? 200.0 : s * 3.125;
    return linearOctaves(percent / 100.0 * (midinote - 60) / 12.0);
}

double extFilterVelocityRatio(int sensitivity, int velocity) {
    // cents at sensitivity +63 by velocity (measured); negative sensitivities mirror the
    // velocity (128 - velocity). Downward moves scale with the sensitivity in cents, upward
    // ones in frequency ratio (measured at -64, -32, +16, +32, +63)
    static const double vx[] = { 1, 40, 80, 100, 110, 113, 116, 120, 127 };
    static const double vc[] = { -9609, -6248, -2842, -1274, -375, -130, 103, 617, 1201 };
    const int s = std::clamp(sensitivity, -64, 63);
    if (s == 0)
        return 1.0;
    const int vel = s > 0 ? std::clamp(velocity, 1, 127) : 128 - std::clamp(velocity, 1, 127);
    const double c = interpolate(vx, vc, vel), share = (s > 0 ? s / 63.0 : -s / 64.0);
    return c <= 0 ? std::exp2(share * c / 1200) : 1 + share * (std::exp2(c / 1200) - 1);
}

double extFilterLpf18sRatio(double value) {
    // LPF18s: the resonant section's F over the one-pole's, by cutoff value (fitted at
    // resonance 64 every 64 values from 128 to 640; it scatters between 9.5 and 10.4 with no
    // trend found, so it is kept per measured value, straight between, held at the ends)
    static const double x[] = { 128, 192, 256, 320, 384, 448, 512, 576, 640 };
    static const double r[] = { 9.799, 10.225, 9.514, 10.031, 9.896, 9.646, 10.378, 9.501, 9.821 };
    return interpolate(x, r, value);
}

void extFilterWidth(int resonance, double &high, double &low) {
    // BPFw: its edges move apart by 3.207 cutoff values per step, the upper one starting 31.5
    // values late and the lower one 19.7 values early (fitted at cutoff 512, widths 0-127)
    const int r = std::clamp(resonance, 0, 127);
    high = std::max(0.0, 3.207 * r - 31.5);
    low = std::max(0.0, 3.207 * r - 19.7);
}

double extFilterGain(int gain) {
    // 0.375 dB per step below 255 (measured 0, 64, 128, 192, 255 through Thru and HPF12); 0 is silent
    const int g = std::clamp(gain, 0, 255);
    return g == 0 ? 0.0 : std::pow(10.0, 0.375 * (g - 255) / 20.0);
}

double extFegLevelCents(int level) { return 75.0 * std::clamp(level, -128, 127); } // measured -32..+32 at depth +63

double extFegDepth(int depth) {
    // the share of the FEG's level that reaches the cutoff (measured at -64, -32, 0, 16, 32,
    // 48 and 63 with level +16; between, straight)
    static const double x[] = { -64, -32, 0, 16, 32, 48, 63 };
    static const double y[] = { -1.0, -0.497, 0.0, 0.274, 0.505, 0.704, 1.004 };
    return interpolate(x, y, std::clamp(depth, -64, 63));
}

// the filter EG's times: a segment takes its time value's duration whatever its distance;
// measured at every second value from 20 to 92 and at 100 and 108 (from 96 up all the same),
// holds at every fourth value; between, straight
double extFegSegmentMs(int time) { return interpolate(kFegSegmentX, kFegSegmentMs, std::clamp(time, 0, 127)); }
double extFegHoldMs(int time) { return interpolate(kFegHoldX, kFegHoldMs, std::clamp(time, 0, 127)); }
double extFilterResonanceVelocityQ(int sensitivity, int velocity) {
    // the Q's divisor grows by h(127 - velocity) times the sensitivity's share (measured at +63
    // with velocities 64-127, +32 at 96 and 120, -64 at 1, 8 and 32; negative sensitivities
    // mirror the velocity); straight beyond the last measured point
    static const double x[] = { 0, 7, 17, 31, 63, 126 };
    static const double h[] = { 0, 0.25, 0.5625, 1.0, 3.0, 6.9375 };
    const int s = std::clamp(sensitivity, -64, 63);
    if (s == 0)
        return 1.0;
    const int vel = s > 0 ? std::clamp(velocity, 1, 127) : 128 - std::clamp(velocity, 1, 127);
    return 1.0 / (1.0 + (s > 0 ? s / 63.0 : -s / 64.0) * interpolate(x, h, 127 - vel));
}

double extFilterScalingRatio(double offset) { return std::exp2(offset * 37.5 / 1200); } // 64 steps: 2 octaves

double extFegDepthVelocity(int sensitivity, int curve, int velocity) {
    // the share of the FEG depth by velocity at sensitivity +63, per curve (measured: curve 2
    // at velocities 1, 32, 64, 96 and 127; the others at 32 and 96, taken through curve 2's
    // ends); between, straight; other sensitivities in proportion, negative ones mirror the
    // velocity
    static const double vx[] = { 1, 32, 64, 96, 127 };
    static const double g[5][5] = {
        { 0, 0.427, 0.543, 0.659, 1.142 }, { 0, 0.074, 0.338, 0.602, 1.142 }, { 0, 0.296, 0.548, 0.783, 1.142 },
        { 0, 0.485, 0.772, 1.059, 1.142 }, { 0, 0.149, 0.559, 0.968, 1.142 } };
    const int s = std::clamp(sensitivity, -64, 63);
    if (s == 0)
        return 1.0;
    const int vel = s > 0 ? std::clamp(velocity, 1, 127) : 128 - std::clamp(velocity, 1, 127);
    const double share = (s > 0 ? s / 63.0 : -s / 64.0);
    return 1.0 + share * (interpolate(vx, g[std::clamp(curve, 0, 4)], vel) - 1.0);
}

double extFegTimeKeyFollowScale(int sensitivity, int midinote, int centre) {
    // +63 two octaves above the centre: 12 times shorter (measured); straight in octaves
    // shortcut: negative sensitivities measured shorter too (-64 two octaves above the
    // centre: instant), not modelled; they mirror the positive rule here
    return std::exp2(-1.79 * std::clamp(sensitivity, -64, 63) / 63.0 * (midinote - centre) / 12.0);
}

// 68 cents per step at the LFO's peak (measured at depth 25, about +-1700 cents with a triangle
// LFO; deeper settings run into the cutoff's limits in the probes)
// shortcut: straight in depth from one measured depth; measure 5-20 with a lower cutoff
double extFilterLfoOctaves(int depth) { return std::clamp(depth, 0, 99) * 68.0 / 1200.0; }

// ---- the common pitch EG (fm/fit_peg.py) ----

// Its level in 37.5-cent steps at depth 8 octaves: one step per level to +-32, then wider
// (measured at every level at depth 2 octaves, each within 0.32 cents of the step grid)
static const int16_t kPartPegSteps[101] = {
    -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43, -41, -39, -37,
    -35, -33, -32, -31, -30, -29, -28, -27, -26, -25, -24, -23, -22, -21, -20, -19,
    -18, -17, -16, -15, -14, -13, -12, -11, -10, -9, -8, -7, -6, -5, -4, -3,
    -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 38, 40, 43, 46, 49, 53, 58, 65, 73, 82,
    92, 103, 115, 127, 128,
};

double extPartPegCents(int level, int depth) {
    static const double scale[4] = { 1.0, 0.25, 0.125, 0.0625 }; // 8, 2, 1, 0.5 octaves
    return kPartPegSteps[clampi(level, -50, 50) + 50] * 37.5 * scale[clampi(depth, 0, 3)];
}

double extPartPegVelocityScale(int sensitivity, int velocity) {
    // the levels lose this many of their 128 steps at +50 at velocity 1, in proportion to
    // 127 - velocity below that, rounded down (measured: every sensitivity at velocity 1 and
    // 7 and 3 at four and two more velocities, each within 0.3 cents at depth 1 octave)
    static const int lost[8] = { 0, 17, 35, 53, 70, 88, 106, 126 };
    const int v = clampi(velocity, 1, 127);
    return (128 - lost[clampi(sensitivity, 0, 7)] * (127 - v) / 126) / 128.0;
}

double extPartPegTimeKeyFollowScale(int sensitivity, int midinote) {
    // rates times 1 + sensitivity (note - 3) / 33 (measured at 0-7 and notes 24-108 every
    // octave, within 0.6 %)
    // shortcut: below note 3 the rate is left unchanged (unmeasured)
    return 1.0 / (1.0 + clampi(sensitivity, 0, 7) * std::max(0, midinote - 3) / 33.0);
}

// Every segment's rate is a whole multiple of 0.05619077 cents per ms (each measured within
// 0.05 % of it, from decays across 2400 cents at every time value; the same at every depth);
// time 0 is instant.
static const uint8_t kPartPegRateSteps[100] = {
    0, 254, 244, 233, 212, 203, 194, 186, 179, 172, 166, 160, 154, 148, 142, 136, 131, 125, 120, 115,
    110, 106, 102, 98, 94, 91, 88, 85, 82, 79, 76, 74, 72, 70, 68, 66, 64, 62, 60, 58,
    56, 54, 53, 51, 49, 47, 46, 44, 42, 41, 39, 38, 37, 36, 34, 33, 31, 30, 28, 27,
    26, 25, 24, 23, 22, 21, 20, 19, 18, 18, 17, 16, 16, 15, 14, 14, 13, 13, 12, 12,
    11, 11, 10, 10, 9, 9, 8, 8, 7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 1,
};
double extPartPegCentsPerMs(int time) { return kPartPegRateSteps[clampi(time, 0, 99)] * 0.05619077; }

// ---- pitch key follow and random pitch ----

// Pitch key follow by value (percent): 1 % steps within 10 of 0, 100 and 200 (and down to
// 191 at the bottom), 5 % between (measured at every value two octaves above the centre,
// each within 1 cent of its whole percent)
static const int16_t kPitchKeyFollowPercent[128] = {
    -200, -199, -198, -197, -196, -195, -194, -193, -192, -191, -190, -185, -180, -175, -170, -165,
    -160, -155, -150, -145, -140, -135, -130, -125, -120, -115, -110, -105, -104, -103, -102, -101,
    -100, -99, -98, -97, -96, -95, -90, -85, -80, -75, -70, -65, -60, -55, -50, -45,
    -40, -35, -30, -25, -20, -15, -10, -9, -8, -7, -6, -5, -4, -3, -2, -1,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 35,
    40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 96, 97, 98, 99,
    100, 101, 102, 103, 104, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150, 155,
    160, 165, 170, 175, 180, 185, 190, 192, 193, 194, 195, 196, 197, 198, 199, 200,
};

int extPitchKeyFollowPercent(int value) { return kPitchKeyFollowPercent[clampi(value, 0, 127)]; }

// straight in cents from the centre note (measured at fractional octaves, within 1 cent)
double extPitchKeyFollowCents(int value, int midinote, int centre) {
    return (extPitchKeyFollowPercent(value) - 100) * (midinote - clampi(centre, 0, 127));
}

double extRandomPitchCents(int depth, uint32_t random) {
    // whole steps of 18.75 cents (the reference's offsets lay on that grid within 1.4 cents): a
    // uniform value within depth / 4 steps either way, cut towards 0 (`m7f`, 32-121 notes per
    // depth: depth 4 always played 0, 8 reached one step, 16 three, 32 seven or eight, 64 about 16,
    // 127 about 31; the reference reached one step further down than up at 32, 64 and 127, which
    // this leaves out)
    const double u = (random >> 8) * (2.0 / 16777216.0) - 1.0; // -1 <= u < 1
    return (double)(int)(u * clampi(depth, 0, 127) / 4.0) * 18.75;
}

// ---- key-on delay ----

// Key-on delay by value: whole 64-sample blocks at 44.1 kHz (measured at every value against a
// second part without delay, each within 0.07 blocks of the grid; 0 to 8.08 s)
static const uint16_t kKeyOnDelayBlocks[128] = {
    0, 10, 15, 20, 39, 46, 57, 75, 91, 108, 126, 148, 168, 186, 206, 227,
    248, 280, 296, 325, 357, 371, 401, 430, 464, 492, 515, 544, 582, 606, 643, 679,
    703, 740, 775, 809, 844, 875, 916, 955, 986, 1027, 1065, 1100, 1137, 1182, 1211, 1252,
    1298, 1337, 1376, 1420, 1460, 1502, 1545, 1591, 1635, 1676, 1721, 1762, 1814, 1863, 1901, 1952,
    1994, 2041, 2089, 2133, 2181, 2234, 2276, 2329, 2380, 2433, 2476, 2523, 2580, 2626, 2684, 2731,
    2790, 2836, 2887, 2947, 3004, 3050, 3106, 3159, 3211, 3269, 3324, 3376, 3437, 3487, 3547, 3604,
    3660, 3712, 3778, 3831, 3896, 3948, 4009, 4069, 4125, 4192, 4245, 4308, 4367, 4432, 4494, 4554,
    4614, 4679, 4740, 4803, 4862, 4923, 4987, 5047, 5119, 5178, 5245, 5313, 5370, 5443, 5506, 5570,
};
double extKeyOnDelayMs(int time) { return kKeyOnDelayBlocks[clampi(time, 0, 127)] * 64 / 44.1; }

double extKeyOnDelayBeats(int note) {
    // 16th, 8th triplet, dotted 16th, 8th, quarter triplet, dotted 8th, quarter, half triplet,
    // dotted quarter, half, whole triplet, dotted half, then 4-8 quarters
    static const double beats[17] = { 0.25, 1 / 3.0, 0.375, 0.5, 2 / 3.0, 0.75, 1, 4 / 3.0, 1.5, 2, 8 / 3.0, 3, 4, 5, 6, 7, 8 };
    return beats[clampi(note, 0, 16)];
}

// ---- pan ----

void extPanGains(int position, float &left, float &right) {
    // the far side's gain by distance from the centre, every 4 positions (measured with
    // alternate pan, the same both ways within 0.02 dB); the near side keeps full level
    // (constant at the centre: no pan law dip); nothing from 63 on; straight in amplitude between
    static const float g[17] = { 1.0f, 0.99916f, 0.95598f, 0.91621f, 0.83947f, 0.7698f, 0.67598f, 0.56891f, 0.47799f,
                                 0.3689f, 0.28422f, 0.20131f, 0.13042f, 0.07105f, 0.02864f, 0.00528f, 0.0f };
    const int p = clampi(position, -64, 63), d = p < 0 ? -p : p;
    float far;
    if (d >= 63)
        far = 0;
    else if (d >= 60)
        far = g[15] * (63 - d) / 3.0f;
    else
        far = g[d / 4] + (g[d / 4 + 1] - g[d / 4]) * (float)(d % 4) / 4.0f;
    left = p > 0 ? far : 1.0f;
    right = p < 0 ? far : 1.0f;
}

int extScalingPanPosition(int depth, int midinote) {
    // depth / 32 positions per key from note 60 down and from note 64 up, none between; +63
    // plays as 64, two per key (measured at depths +63 and +32, notes 24-108 every 6)
    const int n = midinote < 60 ? midinote - 60 : midinote > 64 ? midinote - 64 : 0;
    const int d = clampi(depth, -64, 63);
    return (int)std::lround((d == 63 ? 64 : d) * n / 32.0);
}

int extRandomPanPosition(int depth, uint32_t random) {
    // uniform within depth / 2 positions either way (`m7f`, 32-128 notes per depth: the
    // reference reached about +-58 at 127, +-34 at 64, +-16 at 32, +-8 to 12 at 16); shortcut: its
    // spread at 127 was narrower than uniform (standard deviation 28 positions against 36) and leaned
    // left (mean -7.5), neither modelled
    const double u = (random >> 8) * (2.0 / 16777216.0) - 1.0;
    return (int)std::lround(u * clampi(depth, 0, 127) / 2.0);
}

// ---- the part LFO ----

// Speed by value, Hz (measured at every value: a triangle fitted to the pitch it moves, from
// whole cycles above 50; speed 0 stood still for 20 s, speed 1 from the slope of a 20 s rise)
static const double kPartLfoHz[64] = {
    0, 0.00193, 0.0617357, 0.0673294, 0.0733623, 0.0799999, 0.0872035, 0.0950541,
    0.103675, 0.113039, 0.123234, 0.134347, 0.14652, 0.159664, 0.174096, 0.189761,
    0.206914, 0.225567, 0.245929, 0.268086, 0.292265, 0.31858, 0.347287, 0.378543,
    0.412699, 0.449924, 0.490483, 0.534823, 0.583041, 0.635359, 0.692989, 0.755392,
    0.823748, 0.897714, 0.979524, 1.06707, 1.16296, 1.26842, 1.38253, 1.50683,
    1.64324, 1.79076, 1.95106, 2.13021, 2.32028, 2.52856, 2.75859, 3.0068,
    3.27771, 3.57596, 3.89713, 4.25193, 4.65818, 5.02516, 5.51046, 5.97692,
    6.52348, 7.10192, 7.747, 8.44421, 9.2199, 10.0336, 10.9748, 11.8615,
};
double extPartLfoHz(int speed) { return kPartLfoHz[clampi(speed, 0, 63)]; }

double extPartLfoTempoBeats(int length) {
    // 16th, 8th triplet, dotted 16th, 8th, quarter triplet, dotted 8th, quarter, half triplet,
    // dotted quarter, half, whole triplet, dotted half, then 4-8, 16, 32 and 64 quarters
    static const double beats[20] = { 0.25, 1 / 3.0, 0.375, 0.5, 2 / 3.0, 0.75, 1, 4 / 3.0, 1.5, 2, 8 / 3.0, 3,
                                      4, 5, 6, 7, 8, 16, 32, 64 };
    return beats[clampi(length, 0, 19)];
}

// The LFO's delay, fade in, hold and fade out share one time table (the four, measured at every
// eighth value, agree within the measurement's resolution from 48 up): ms by value, from the
// delay's onset at every value (an LFO at 11.9 Hz, every eighth value also at 3.9 Hz), made
// non-decreasing where the fastest values scatter by about 10 ms (largest change 7.6 ms)
// shortcut: the fades and hold below about 100 ms are taken from the delay
static const uint16_t kPartLfoTimeMs[128] = {
    0, 41, 41, 41, 41, 41, 45, 45, 45, 53, 53, 53, 53, 53, 53, 56,
    56, 56, 59, 74, 74, 74, 74, 74, 74, 74, 91, 91, 91, 91, 94, 97,
    97, 99, 126, 126, 126, 130, 130, 139, 151, 151, 152, 175, 175, 176, 176, 211,
    211, 211, 221, 258, 258, 259, 291, 299, 299, 326, 345, 369, 369, 395, 414, 420,
    452, 472, 511, 540, 561, 585, 626, 653, 680, 715, 762, 789, 828, 866, 921, 963,
    1024, 1076, 1112, 1181, 1250, 1303, 1375, 1463, 1524, 1613, 1685, 1770, 1871, 1968, 2081, 2182,
    2306, 2438, 2562, 2685, 2832, 2975, 3132, 3309, 3482, 3662, 3862, 4065, 4300, 4515, 4774, 5015,
    5264, 5559, 5854, 6164, 6492, 6836, 7185, 7579, 7978, 8408, 8856, 9330, 9849, 10366, 10907, 11509,
};
static double partLfoTimeMs(int time) { return kPartLfoTimeMs[clampi(time, 0, 127)]; }
double extPartLfoDelayMs(int time) { return partLfoTimeMs(time); }
double extPartLfoFadeInMs(int time) { return partLfoTimeMs(time); }
double extPartLfoHoldMs(int time) { return partLfoTimeMs(time); }
double extPartLfoFadeOutMs(int time) { return partLfoTimeMs(time); }
// operator frequency destination: 3600 / 127 cents per depth step (measured at 17 depths, within
// 0.3 cents)
double extPartLfoPitchCents(int depth) { return clampi(depth, 0, 127) * 3600.0 / 127; }

// operator level destination: 0.589 dB per depth step at the LFO's bottom, nothing at its top
// (measured with a square wave at depths 8, 32 and 64; 127 silent); shortcut: straight between,
// the reference's level at values between the top and bottom was not measured
double extPartLfoLevelDb(int depth) { return clampi(depth, 0, 127) * 0.589; }

// pan destination: 64 (1 - e^(-depth / 20)) positions at the LFO's top (fitted to a square wave
// at depths 8, 32 and 64 through the pan law: positions 21, 51 and 61; 127 silent on the far
// side); shortcut: the LFO's value scales the positions linearly (values between the top and
// bottom were not measured), and the pan moves once per 64-sample block
double extPartLfoPanPositions(int depth) { return 64.0 * (1.0 - std::exp(-clampi(depth, 0, 127) / 20.0)); }

// Part velocity sensitivity: depth / 64 times the velocity, plus twice the offset's distance from
// 64 (measured at 40 depth, offset and velocity combinations through a carrier's level velocity
// sensitivity; fractional results play between the neighbouring velocities' levels there,
// shortcut: Octet rounds them); a velocity of 0 or below plays no note
double extPartVelocity(int depth, int offset, int velocity) {
    return std::min(127.0, velocity * clampi(depth, 0, 127) / 64.0 + 2.0 * (clampi(offset, 0, 127) - 64));
}

// Part AEG offsets: 1.5 time or level values per step (attack, decay, sustain and release, each
// at 12 offsets on a lone carrier, within 0.4 dB rms of the equivalent EG values); shortcut: the
// rounding of odd offsets is not measured
int extAegOffsetSteps(int offset) { return (int)std::lround(1.5 * clampi(offset, -64, 63)); }

// the attack offset multiplies the attack time by 2^(offset / 4), held within attack times 0-99
// (measured at every second offset from -32 to +32 on a lone carrier at attack 50: from -12 to +26
// the equivalent attack value is within one value of this time's; below -12 the reference is
// faster, at +28 and +30 it reaches attack 99 sooner)
double extAegAttackScale(int offset) { return std::exp2(clampi(offset, -64, 63) / 4.0); }

// the sustain offset: decay 2 level steps measured at every second offset from -22 to +32 (a lone
// carrier at decay 2 level 50; the reference moves the level by even steps, 1.5 per offset step
// on average), straight between; 1.5 per step beyond -22 (shortcut: unmeasured there)
int extAegSustainSteps(int offset) {
    static const double up[17] = { 0, 2, 6, 8, 12, 14, 18, 22, 24, 28, 30, 34, 36, 40, 44, 47, 49 };
    static const double down[12] = { 0, 4, 6, 10, 12, 16, 18, 22, 26, 28, 31, 33 };
    const int v = clampi(offset, -64, 63), a = std::abs(v);
    auto at = [](const double *t, int n, int x) {
        if (x >= 2 * (n - 1))
            return t[n - 1] + 1.5 * (x - 2 * (n - 1));
        return t[x / 2] + (x % 2 ? 0.5 * (t[x / 2 + 1] - t[x / 2]) : 0.0);
    };
    return (int)std::lround(v >= 0 ? at(up, 17, a) : -at(down, 12, a));
}

// Part detune: cents, not Hz (the same cents at notes 36 and 72); measured every eighth value,
// nothing within 2 of the centre, straight between
static const float kPartDetuneCents[33] = {
    -49.95f, -45.94f, -42.81f, -39.95f, -37.11f, -33.99f, -30.59f, -27.76f, -24.65f, -20.71f, -17.62f,
    -14.81f, -11.72f, -8.93f, -6.13f, -3.06f, 0.0f, 2.78f, 5.83f, 8.6f, 11.64f, 14.4f, 17.44f, 20.46f,
    24.58f, 27.32f, 30.33f, 33.88f, 36.88f, 39.6f, 42.59f, 45.57f, 49.63f,
};
double extPartDetuneCents(int detune) {
    const int raw = clampi(detune, -128, 127) + 128;
    if (raw >= 126 && raw <= 130)
        return 0.0;
    if (raw > 120 && raw < 126)
        return kPartDetuneCents[15] * (126 - raw) / 6.0;
    if (raw > 130 && raw < 136)
        return kPartDetuneCents[17] * (raw - 130) / 6.0;
    if (raw == 255)
        return kPartDetuneCents[32];
    const int i = raw / 8;
    const double t = (raw - 8 * i) / (i == 31 ? 7.0 : 8.0);
    return kPartDetuneCents[i] + (kPartDetuneCents[i + 1] - kPartDetuneCents[i]) * t;
}

// The ratio macro (Ratio Shift): the ratio a modulator plays at every third and every eleventh
// macro value for nine ratios (`m8b`, `m8d`: coarse 0-4, 8 and 16, coarse 1 fine 50, coarse 4
// fine 25), read from the sideband spacing. Neither a factor nor an offset: above ratio 4 the
// shift is nearly the same offset at every ratio (+17.3 at +99, -2.15 at -99), below it smaller;
// never below 0.5. shortcut: straight between the measured ratios and values (the reference's
// curve is not smooth between them); above ratio 16 the offset at 16
static const int kRatioShiftValue[79] = {
    -99, -96, -93, -90, -88, -87, -84, -81, -78, -77, -75, -72, -69, -66, -63, -60, -57, -55, -54,
    -51, -48, -45, -44, -42, -39, -36, -33, -30, -27, -24, -22, -21, -18, -15, -12, -11, -9, -6, -3,
    0, 3, 6, 9, 11, 12, 15, 18, 21, 22, 24, 27, 30, 33, 36, 39, 42, 44, 45, 48, 51, 54, 55, 57, 60,
    63, 66, 69, 72, 75, 77, 78, 81, 84, 87, 88, 90, 93, 96, 99
};
static const double kRatioShiftAt[9] = { 0.5, 1, 1.5, 2, 3, 4, 5, 8, 16 };
static const float kRatioShifted[9][79] = {
    { // 0.5
        0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
        0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
        0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.51f, 0.52f, 0.53f, 0.545f,
        0.55f, 0.575f, 0.595f, 0.63f, 0.65f, 0.685f, 0.74f, 0.8f, 0.855f, 0.95f, 1.119f, 1.35f,
        1.5f, 1.569f, 1.799f, 2.039f, 2.5f, 2.639f, 3.059f, 3.599f, 4.199f, 4.96f, 5.55f, 6.152f,
        6.879f, 7.281f, 7.491f, 8.098f, 8.801f, 10.022f, 10.622f, 11.679f, 13.308f, 14.941f, 16.527f
    },
    { // 1
        0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.53f, 0.585f, 0.64f, 0.65f, 0.665f, 0.685f, 0.71f,
        0.73f, 0.755f, 0.775f, 0.785f, 0.79f, 0.795f, 0.805f, 0.82f, 0.83f, 0.835f, 0.84f, 0.85f,
        0.865f, 0.875f, 0.885f, 0.895f, 0.91f, 0.915f, 0.92f, 0.93f, 0.94f, 0.955f, 0.955f, 0.965f,
        0.977f, 0.982f, 1.0f, 1.018f, 1.04f, 1.06f, 1.09f, 1.1f, 1.15f, 1.19f, 1.259f, 1.3f, 1.37f,
        1.48f, 1.6f, 1.71f, 1.9f, 2.239f, 2.699f, 3.0f, 3.08f, 3.359f, 3.621f, 3.901f, 3.981f,
        4.359f, 5.1f, 5.668f, 6.299f, 7.019f, 7.618f, 8.252f, 8.717f, 8.956f, 9.551f, 10.199f,
        11.522f, 12.0f, 13.129f, 14.761f, 16.35f, 17.938f
    },
    { // 1.5
        0.5f, 0.555f, 0.61f, 0.665f, 0.705f, 0.725f, 0.78f, 0.835f, 0.89f, 0.9f, 0.915f, 0.935f,
        0.96f, 0.981f, 1.007f, 1.05f, 1.07f, 1.08f, 1.09f, 1.11f, 1.14f, 1.16f, 1.17f, 1.18f, 1.2f,
        1.23f, 1.25f, 1.27f, 1.29f, 1.32f, 1.319f, 1.359f, 1.36f, 1.38f, 1.41f, 1.41f, 1.43f, 1.45f,
        1.47f, 1.5f, 1.52f, 1.54f, 1.56f, 1.59f, 1.6f, 1.65f, 1.69f, 1.76f, 1.799f, 1.87f, 1.981f,
        2.2f, 2.42f, 2.799f, 3.139f, 3.421f, 3.599f, 3.68f, 3.959f, 4.321f, 4.708f, 4.831f, 5.159f,
        5.73f, 6.398f, 7.082f, 7.681f, 8.33f, 9.03f, 9.442f, 9.63f, 10.262f, 10.982f, 12.203f,
        12.779f, 13.805f, 15.465f, 17.095f, 18.72f
    },
    { // 2
        0.75f, 0.805f, 0.86f, 0.915f, 0.955f, 0.978f, 1.06f, 1.17f, 1.28f, 1.3f, 1.319f, 1.37f,
        1.42f, 1.46f, 1.51f, 1.549f, 1.569f, 1.58f, 1.59f, 1.61f, 1.64f, 1.66f, 1.67f, 1.679f, 1.7f,
        1.73f, 1.75f, 1.77f, 1.79f, 1.82f, 1.83f, 1.84f, 1.86f, 1.88f, 1.911f, 1.91f, 1.93f, 1.943f,
        1.996f, 2.0f, 2.047f, 2.08f, 2.12f, 2.179f, 2.2f, 2.299f, 2.38f, 2.519f, 2.601f, 2.74f,
        2.96f, 3.12f, 3.24f, 3.479f, 3.74f, 4.037f, 4.28f, 4.4f, 4.798f, 5.121f, 5.402f, 5.501f,
        5.799f, 6.481f, 7.149f, 7.749f, 8.398f, 9.103f, 9.702f, 10.104f, 10.346f, 11.041f, 11.64f,
        12.961f, 13.499f, 14.563f, 16.162f, 17.781f, 19.404f
    },
    { // 3
        1.01f, 1.119f, 1.23f, 1.359f, 1.42f, 1.46f, 1.569f, 1.679f, 1.79f, 1.81f, 1.84f, 1.881f,
        1.931f, 1.991f, 2.032f, 2.12f, 2.16f, 2.179f, 2.2f, 2.239f, 2.299f, 2.34f, 2.361f, 2.38f,
        2.42f, 2.48f, 2.519f, 2.56f, 2.601f, 2.659f, 2.679f, 2.699f, 2.74f, 2.779f, 2.84f, 2.84f,
        2.88f, 2.92f, 2.959f, 3.0f, 3.03f, 3.059f, 3.08f, 3.12f, 3.12f, 3.179f, 3.24f, 3.32f,
        3.359f, 3.449f, 3.58f, 3.72f, 3.859f, 4.121f, 4.499f, 4.89f, 5.1f, 5.201f, 5.491f, 5.761f,
        6.081f, 6.198f, 6.56f, 7.242f, 7.838f, 8.501f, 9.177f, 9.78f, 10.437f, 10.9f, 11.108f,
        11.702f, 12.396f, 13.684f, 14.183f, 15.301f, 16.935f, 18.53f, 20.139f
    },
    { // 4
        1.84f, 1.946f, 2.12f, 2.34f, 2.5f, 2.579f, 2.799f, 3.0f, 3.139f, 3.16f, 3.199f, 3.24f, 3.3f,
        3.359f, 3.421f, 3.46f, 3.479f, 3.501f, 3.51f, 3.541f, 3.57f, 3.599f, 3.599f, 3.621f, 3.641f,
        3.68f, 3.7f, 3.72f, 3.751f, 3.779f, 3.799f, 3.809f, 3.841f, 3.859f, 3.901f, 3.901f, 3.92f,
        3.941f, 3.959f, 4.0f, 4.042f, 4.079f, 4.109f, 4.16f, 4.168f, 4.26f, 4.321f, 4.439f, 4.521f,
        4.639f, 4.831f, 5.011f, 5.159f, 5.398f, 5.68f, 5.972f, 6.202f, 6.32f, 6.699f, 7.039f,
        7.351f, 7.441f, 7.738f, 8.398f, 9.091f, 9.681f, 10.318f, 11.003f, 11.622f, 12.039f, 12.254f,
        12.961f, 13.581f, 14.882f, 15.404f, 16.493f, 18.085f, 19.707f, 21.332f
    },
    { // 5
        2.84f, 3.03f, 3.16f, 3.3f, 3.391f, 3.44f, 3.57f, 3.7f, 3.841f, 3.859f, 3.902f, 3.941f, 4.0f,
        4.079f, 4.16f, 4.231f, 4.26f, 4.28f, 4.288f, 4.321f, 4.379f, 4.409f, 4.439f, 4.439f, 4.478f,
        4.53f, 4.561f, 4.598f, 4.639f, 4.68f, 4.708f, 4.721f, 4.76f, 4.798f, 4.841f, 4.841f, 4.88f,
        4.92f, 4.951f, 5.0f, 5.011f, 5.038f, 5.069f, 5.1f, 5.121f, 5.19f, 5.24f, 5.319f, 5.369f,
        5.46f, 5.599f, 5.761f, 5.902f, 6.181f, 6.539f, 6.902f, 7.121f, 7.203f, 7.501f, 7.802f,
        8.12f, 8.242f, 8.6f, 9.252f, 9.853f, 10.53f, 11.198f, 11.804f, 12.481f, 12.954f, 13.129f,
        13.758f, 14.406f, 15.699f, 16.206f, 17.306f, 18.961f, 20.553f, 22.201f
    },
    { // 8
        5.851f, 6.0f, 6.181f, 6.359f, 6.481f, 6.551f, 6.717f, 6.902f, 7.072f, 7.082f, 7.14f, 7.198f,
        7.252f, 7.301f, 7.36f, 7.42f, 7.441f, 7.451f, 7.481f, 7.501f, 7.553f, 7.558f, 7.563f,
        7.598f, 7.618f, 7.65f, 7.681f, 7.702f, 7.738f, 7.77f, 7.802f, 7.802f, 7.838f, 7.85f, 7.902f,
        7.902f, 7.919f, 7.951f, 7.978f, 8.0f, 8.049f, 8.081f, 8.103f, 8.158f, 8.158f, 8.252f, 8.32f,
        8.398f, 8.479f, 8.6f, 8.758f, 8.956f, 9.103f, 9.36f, 9.662f, 9.961f, 10.199f, 10.303f,
        10.645f, 10.987f, 11.306f, 11.406f, 11.702f, 12.362f, 13.041f, 13.656f, 14.298f, 14.982f,
        15.604f, 16.0f, 16.239f, 16.923f, 17.542f, 18.871f, 19.362f, 20.442f, 22.051f, 23.676f,
        25.301f
    },
    { // 16
        13.861f, 14.003f, 14.165f, 14.319f, 14.483f, 14.522f, 14.702f, 14.851f, 15.003f, 15.044f,
        15.105f, 15.126f, 15.206f, 15.28f, 15.342f, 15.393f, 15.404f, 15.434f, 15.455f, 15.475f,
        15.509f, 15.54f, 15.571f, 15.591f, 15.604f, 15.614f, 15.676f, 15.699f, 15.72f, 15.751f,
        15.763f, 15.805f, 15.838f, 15.838f, 15.902f, 15.902f, 15.923f, 15.943f, 15.967f, 16.0f,
        16.054f, 16.075f, 16.108f, 16.162f, 16.162f, 16.239f, 16.306f, 16.383f, 16.483f, 16.56f,
        16.74f, 16.923f, 17.085f, 17.342f, 17.638f, 17.938f, 18.195f, 18.306f, 18.606f, 18.961f,
        19.26f, 19.362f, 19.681f, 20.344f, 21.002f, 21.609f, 22.262f, 22.951f, 23.563f, 24.0f,
        24.211f, 24.889f, 25.506f, 26.833f, 27.368f, 28.406f, 30.026f, 31.676f, 33.347f
    },
};
double extShiftedRatio(double ratio, int value) {
    value = clampi(value, -99, 99);
    if (value == 0)
        return ratio;
    constexpr int last = (int)(sizeof kRatioShiftValue / sizeof kRatioShiftValue[0]) - 1;
    int i = 0;
    while (i < last - 1 && kRatioShiftValue[i + 1] <= value)
        i++;
    const double u = (double)(value - kRatioShiftValue[i]) / (kRatioShiftValue[i + 1] - kRatioShiftValue[i]);
    auto offset = [&](int j) {
        return kRatioShifted[j][i] + (kRatioShifted[j][i + 1] - kRatioShifted[j][i]) * u - kRatioShiftAt[j];
    };
    double d;
    if (ratio <= kRatioShiftAt[0]) {
        d = offset(0);
    } else if (ratio >= kRatioShiftAt[8]) {
        d = offset(8);
    } else {
        int j = 0;
        while (kRatioShiftAt[j + 1] < ratio)
            j++;
        const double t = (ratio - kRatioShiftAt[j]) / (kRatioShiftAt[j + 1] - kRatioShiftAt[j]);
        d = offset(j) + (offset(j + 1) - offset(j)) * t;
    }
    return std::max(0.5, ratio + d);
}

// Portamento: every glide moves straight in semitones. Rate 1 keeps the time value's rate
// whatever the distance; time 1 takes as long for any distance as rate 1 takes for an octave;
// rate 2 and time 2 start at most an octave away (the engine clamps the start), then as rate 1 and
// time 1 (`m7f`, `m7g`: 5, 12, 19, 24 semitones). Rates measured every fourth value (legato
// glides in mono: over 48 semitones up to time 32, 12 or 24 above), straight in log rate between.
static const int kPortaTimes[] = { 1, 2, 4, 6, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64,
                                   68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 127 };
static const double kPortaRate[] = { 434.1, 381.0, 378.3, 336.1, 336.2, 274.2, 248.7, 207.3, 176.3, 151.2,
                                     131.1, 116.3, 94.93, 85.03, 71.99, 58.98, 51.22, 43.10, 36.63, 30.40,
                                     25.63, 21.26, 17.81, 14.66, 11.843, 9.354, 7.533, 5.943, 4.565, 3.402,
                                     2.42, 1.770, 1.083, 0.5208, 0.1199 };
double extPortamentoRate(int time, int timeMode, double semitones) {
    time = clampi(time, 0, 127);
    if (time == 0)
        return 0.0;
    int i = 0;
    while (kPortaTimes[i + 1] < time)
        i++;
    const double t = (double)(time - kPortaTimes[i]) / (kPortaTimes[i + 1] - kPortaTimes[i]);
    const double rate = std::exp(std::log(kPortaRate[i]) + (std::log(kPortaRate[i + 1]) - std::log(kPortaRate[i])) * t);
    if (timeMode == 1 || timeMode == 3)
        return rate * std::min(semitones, timeMode == 3 ? 12.0 : 1e9) / 12.0;
    return rate;
}
