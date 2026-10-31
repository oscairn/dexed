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
    // whole steps of 18.75 cents up to depth / 4 steps either way (+-600 cents at 127); the
    // reference's offsets lay on that grid within 1.4 cents
    // shortcut: uniform over the steps; the reference's distribution was not measured (10
    // notes per depth)
    const int k = (clampi(depth, 0, 127) + 2) / 4;
    return ((int)(random % (uint32_t)(2 * k + 1)) - k) * 18.75;
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
    // shortcut: uniform within 64 (depth / 127)^2 positions either way: the reference's 16
    // notes at 127 reached 50 and 8 notes at 64 reached 14; its distribution (13 of 16 to the
    // left) was not resolved
    const double d = clampi(depth, 0, 127) / 127.0;
    const int range = (int)std::lround(64 * d * d);
    return (int)(random % (uint32_t)(2 * range + 1)) - range;
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

// The LFO's delay, fade in, hold and fade out share one time table (the four measured at every
// eighth value agree within the measurement's resolution from 48 up); measured from the delay's
// onset at every eighth value, straight in log time between (shortcut: the values between and the
// fastest times, below about 100 ms, are not resolved; the fades were read at one LFO cycle's
// resolution, 0.26 s)
static double partLfoTimeMs(int time) {
    static const double x[17] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127 };
    static const double ms[17] = { 0, 31, 55, 67, 95, 152, 207, 294, 450, 680, 1024, 1524, 2306, 3483, 5264, 7978, 11509 };
    const int t = clampi(time, 0, 127);
    int i = 0;
    while (i < 15 && t > x[i + 1])
        i++;
    const double f = (t - x[i]) / (x[i + 1] - x[i]);
    if (i == 0)
        return ms[1] * f;
    return ms[i] * std::pow(ms[i + 1] / ms[i], f);
}
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
