/*
 * The Extended profile's measured tables (docs/oscairn-plan.md, milestone M4).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * The data in ext_tables.cc is fitted to reference renders by the measurement repository
 * (oscairn/measurement, fm/fit_tables.py); docs/oscairn-plan.md section 11 lists what was
 * measured and how well each table fits.
 */
#pragma once

#include <cstdint>

// envelopes
double extEgMsPerDb(int time);      // decay 1, decay 2 and release: ms per dB of level change
double extAttackMsPerDb(int time);  // attack: its rate (ms per dB, as a decay of that time)
double extAttackOverDb();           // attack: its asymptote, in dB above its target
double extAttackStartDb();          // attack: it starts this many dB below its target
double extHoldMs(int time);         // hold segment length
double extEgLevelDb(int level);     // EG level 0-99, dB relative to level 99
double extOpLevelDb(int level);     // operator level 0-99, dB relative to level 99
double extTimeKeyFollowScale(int sensitivity, int midinote); // multiplies every segment time

// level scaling and velocity, dB
double extLevelScalingDb(int midinote, int breakPoint, int lowDepth, int highDepth, int lowCurve, int highCurve);
double extLevelVelocityDb(int sensitivity, int velocity);

// pitch
double extRatio(int coarse, int fine);
double extDetuneCents(int detune, int midinote);
double extFixedHz(int coarse, int fine);
double extKeyFollowCents(int sensitivity, int midinote);  // fixed mode
double extPitchVelocityCents(int sensitivity, int velocity);
double extPegCents(int level);      // operator pitch EG level -50-+50
double extPegMs(int time);          // operator pitch EG: time to cross the full -50-+50 range

// LFO
double extLfoHz(int speed);         // speed range Normal, 0-99
double extLfoExtendedHz(int speed); // speed range Extended, 0-415
int32_t extPitchDepthOffset(int offset); // as DX7 pitch modulation sensitivity
int32_t extAmpDepthOffset(int offset);   // as DX7 amplitude modulation sensitivity (Q24)
