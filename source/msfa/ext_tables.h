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
double extModLevelDb(int level);    // the same for a modulator (its modulation index: 4 pi at 0 dB)
double extFeedbackScale(int feedback); // feedback 1-7, relative to the DX7's amount
double extAlgorithmGainDb(int algorithm); // carriers' output level by algorithm 1-88, dB re algorithm 1
double extTimeKeyFollowScale(int sensitivity, int midinote); // multiplies every segment time

// level scaling and velocity, dB
double extLevelScalingDb(int midinote, int breakPoint, int lowDepth, int highDepth, int lowCurve, int highCurve);
double extLevelVelocityDb(int sensitivity, double velocity);

// pitch
double extRatio(int coarse, int fine);
double extDetuneCents(int detune, int midinote);
double extFixedHz(int coarse, int fine);
double extKeyFollowCents(int sensitivity, int midinote);  // fixed mode
double extPitchVelocityCents(int depth, int sensitivity, int velocity); // common depth, operator sensitivity
double extPegCents(int level);      // operator pitch EG level -50-+50
double extPegCentsPerMs(int time);   // operator pitch EG: a segment's rate, straight in cents
// the common pitch EG
double extPartPegCents(int level, int depth);   // level -50-+50, depth 0-3 (8, 2, 1, 0.5 octaves)
double extPartPegCentsPerMs(int time);           // 0-99: a segment's rate (0: instant)
double extPartPegVelocityScale(int sensitivity, int velocity); // 0-7: multiplies the levels
double extPartPegTimeKeyFollowScale(int sensitivity, int midinote); // 0-7: multiplies segment times
int extPitchKeyFollowPercent(int value);   // pitch key follow 0-127 -> -200 ... +200 %
double extPitchKeyFollowCents(int value, int midinote, int centre); // offset from the note's own pitch
double extRandomPitchCents(int depth, uint32_t random); // random pitch 0-127, random: a uniform 32-bit value
double extKeyOnDelayMs(int time);    // key-on delay 0-127
double extKeyOnDelayBeats(int note); // tempo-synced key-on delay 0-16 (16th ... 4th x 8), in beats
// pan: positions -64 (left) ... +63 (right)
void extPanGains(int position, float &left, float &right);
int extScalingPanPosition(int depth, int midinote); // scaling pan -64-+63
int extRandomPanPosition(int depth, uint32_t random); // random pan 0-127
// the part LFO
double extPartLfoHz(int speed);          // 0-63
double extPartLfoTempoBeats(int length); // tempo sync 0-19 (16th ... 4th x 64), in beats per cycle
double extPartLfoDelayMs(int time);      // 0-127
double extPartLfoFadeInMs(int time);     // 0-127
double extPartLfoHoldMs(int time);       // 0-126
double extPartLfoFadeOutMs(int time);    // 0-127
double extPartLfoPitchCents(int depth);  // operator frequency destination: cents at the LFO's peak, depth 0-127
double extPartLfoLevelDb(int depth);     // operator level destination: dB lost at the LFO's bottom
double extPartLfoPanPositions(int depth); // pan destination: pan positions at the LFO's top

// LFO
double extLfoHz(int speed);         // speed range Normal, 0-99
double extLfoExtendedHz(int speed); // speed range Extended, 0-415
int32_t extPitchDepthOffset(int offset); // as DX7 pitch modulation sensitivity
int32_t extLfoPitchDepthSteps(int depth); // pitch depth 0-99 in the DX7's steps (0-256)
double extLfoPitchCents(int depth, int offset); // the 2nd LFO's pitch swing at its peak, in cents
int32_t extAmpDepthOffset(int offset);   // as DX7 amplitude modulation sensitivity (Q24)

// filter
double extFilterCutoffHz(double value); // cutoff value 0-1023 (fractions interpolate) -> Hz
double extFilterMaxHz();                // the highest cutoff the filter plays
double extFilterMaxDamping(double fc);  // the most damping (1 / Q) the filter allows at a cutoff
double extFilterQ(int resonance);       // resonance 0-127 -> Q
double extFilterKeyFollowRatio(int sensitivity, int midinote); // -64-+63 (cutoff and HPF key follow)
double extFilterVelocityRatio(int sensitivity, int velocity);  // cutoff velocity sensitivity -64-+63
double extFilterResonanceVelocityQ(int sensitivity, int velocity); // multiplies the Q
double extFilterScalingRatio(double offset); // cutoff scaling offset -128-+127 (interpolated by note)
double extFilterLpf18sRatio(double value); // by cutoff value 0-1023
void extFilterWidth(int resonance, double &high, double &low); // BPFw edges, cutoff values from the cutoff
double extFilterGain(int gain);         // 0-255 -> amplitude
double extFilterLfoOctaves(int depth);  // 2nd LFO filter depth 0-99 -> octaves at the LFO's peak
// filter EG
double extFegLevelCents(int level);     // -128-+127
double extFegSegmentMs(int time);       // 0-127: a segment's duration
double extFegHoldMs(int time);          // 0-127
double extFegDepthVelocity(int sensitivity, int curve, int velocity); // multiplies the FEG depth
double extFegTimeKeyFollowScale(int sensitivity, int midinote, int centre); // multiplies FEG times
double extFegDepth(int depth);          // -64-+63: the share of the FEG level (cents) that moves the cutoff
// part values
double extPartVelocity(int depth, int offset, int velocity); // the velocity the voice plays; <= 0: no note
int extAegOffsetSteps(int offset);      // -64-+63: EG time or level steps added to every operator
double extPartDetuneCents(int detune);  // -128-+127
// portamento time 0-127 and time mode 0-3 (rate 1, time 1, rate 2, time 2): the glide's rate in
// semitones per second over a distance of `semitones` (time 0: instant, returns 0)
double extPortamentoRate(int time, int timeMode, double semitones);

