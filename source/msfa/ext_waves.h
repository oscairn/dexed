/*
 * The Extended profile's operator waves (docs/oscairn-plan.md, section 3.7, milestone M5).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <cstdint>

// Waves 1-4 (all harmonics 1 and 2, odd harmonics 1 and 2) are single-cycle tables built from
// measured harmonic amplitudes and phases (ext_waves_data.h), one per spread, each in versions
// holding the first 1-16 components exactly and more in quarter-octave steps up to 512, so a
// note keeps every harmonic below Nyquist up to the 16th. Waves 5-6 (formant 1 and 2) are a window table times a sine at the centre harmonic
// (formant + 1); the window is band-limited the same way. A modulated formant wave plays
// window(t + m) sin(centre t + m): the centre's sine takes the modulation once, not centre times.
namespace extwave {

constexpr int kLgSize = 11;
constexpr int kSize = 1 << kLgSize;

// Builds every table. Call once from a thread that may take time (SynthEngine's constructor).
void init();

// The table of wave 1-4 at a spread, holding no harmonic above maxHarmonic (at least the
// fundamental).
const int32_t *table(int wave, int spread, int maxHarmonic);
// Wave 3-4 as E, the wave being E(2 t) sin(t), holding the wave's harmonics up to maxHarmonic. A
// modulated odd wave plays E(2 t + m) sin(t + m) (`m9a`), not the wave at t + m.
const int32_t *oddWindow(int wave, int spread, int maxHarmonic);
// The window of wave 5-6 at a spread, holding no component above maxComponent (null: none fit).
const int32_t *window(int wave, int spread, int maxComponent);

// Q24 phase (one cycle per 1 << 24), linear interpolation; Q24 out like Sin::lookup.
inline int32_t lookup(const int32_t *t, int32_t phase) {
    constexpr int shift = 24 - kLgSize;
    const uint32_t p = (uint32_t)phase & 0xffffffu;
    const uint32_t i = p >> shift;
    const int32_t frac = (int32_t)(p & ((1u << shift) - 1));
    return t[i] + (int32_t)(((int64_t)(t[i + 1] - t[i]) * frac) >> shift);
}

} // namespace extwave
