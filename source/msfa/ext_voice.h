/*
 * Extended profile voice data (docs/oscairn-plan.md, sections 3 and 5).
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

// The Extended profile's values, as displayed (signed where the range is signed). Classic
// sounds ignore all of it except `profile`; Extended sounds ignore the 156-byte DX7 voice
// except its name.
namespace ext {

enum Profile { Classic = 0, Extended = 1 };

enum Common {
    ProfileSel,  // Profile
    Algorithm,   // 1-88
    Feedback,    // 0-7
    LfoWave,     // 0-5: triangle, saw down, saw up, square, sine, sample and hold
    LfoSpeed,    // 0-99 (speed range Normal)
    LfoPhase,    // 0-3: 0, 90, 180, 270 degrees
    LfoDelay,    // 0-99
    LfoKeyReset, // 0-1
    LfoPitchDepth, // 0-99
    LfoAmpDepth,   // 0-99
    LfoSpeedRange, // 0 Normal, 1 Extended
    LfoExtSpeed,   // 0-415 (speed range Extended)
    PitchVelocityDepth, // -64-+63: velocity to pitch, for every operator (see Op::PitchVelocity)
    // the filter (msfa/ext_filter.h) and its EG
    FilterType,         // 0-18 (extfilter::Type; 18 Thru)
    FilterCutoff,       // 0-1023
    FilterCutoffVelocity, // -64-+63
    FilterResonance,    // 0-127 (BPFw: width)
    FilterResonanceVelocity, // -64-+63
    HpfCutoff,          // 0-1023 (LPF12+HPF12, LPF6+HPF12)
    FilterDistance,     // -128-+127 (dual types)
    FilterGain,         // 0-255
    FegHoldTime, FegAttackTime, FegDecay1Time, FegDecay2Time, FegReleaseTime, // 0-127
    FegHoldLevel, FegAttackLevel, FegDecay1Level, FegDecay2Level, FegReleaseLevel, // -128-+127
    FegDepth,           // -64-+63
    FegTimeVelocitySegment, // 0-4: attack, attack and decay, decay, attack and release, all
    FegTimeVelocity,    // -64-+63
    FegDepthVelocity,   // -64-+63
    FegDepthVelocityCurve, // 0-4
    FegTimeKeyFollow,   // -64-+63
    FegTimeKeyCenter,   // 0-127 (MIDI note)
    FilterKeyFollow,    // -64-+63 (shown as percent)
    FilterBreak1, FilterBreak2, FilterBreak3, FilterBreak4, // 0-127 (MIDI note)
    FilterOffset1, FilterOffset2, FilterOffset3, FilterOffset4, // -128-+127
    HpfKeyFollow,       // -64-+63 (shown as percent)
    LfoFilterDepth,     // 0-99
    // the common pitch EG (every operator; msfa/ext_note.h ExtPartPitchEnv)
    PitchEgInitial, PitchEgAttack, PitchEgDecay1, PitchEgDecay2, PitchEgRelease, // -50-+50
    PitchEgAttackTime, PitchEgDecay1Time, PitchEgDecay2Time, PitchEgReleaseTime, // 0-99
    PitchEgDepth,       // 0-3: 8, 2, 1, 0.5 octaves
    PitchEgVelocity,    // 0-7: depth velocity sensitivity
    PitchEgTimeKeyFollow, // 0-7
    RandomPitch,        // 0-127
    PitchKeyFollow,     // 0-127: -200 % ... +200 % (extPitchKeyFollowPercent; 64 is 0 %, 96 +100 %)
    PitchKeyCentre,     // 0-127 (MIDI note)
    KeyOnDelay,         // 0-127: time before the note starts (tempo sync off)
    KeyOnDelaySync,     // 0-1
    KeyOnDelayNote,     // 0-16: note length with tempo sync (16th ... 4th x 8, extKeyOnDelayBeats)
    RandomPan,          // 0-127
    AlternatePan,       // -64-+63 (left 64 ... right 63)
    ScalingPan,         // -64-+63
    CommonCount
};

enum Op {
    KeyOnReset,   // 0-1
    Mode,         // 0 ratio, 1 fixed
    Coarse,       // 0-31
    Fine,         // 0-127
    Detune,       // -15-+15
    KeyFollow,    // 0-99 (fixed mode)
    PitchVelocity, // -7-+7: this operator's share of the common pitch velocity depth
    PegInitial,   // -50-+50
    PegAttack,    // -50-+50
    PegAttackTime, // 0-99
    PegDecayTime,  // 0-99
    AttackLevel, Decay1Level, Decay2Level, ReleaseLevel, // 0-99 (release level is also the hold level)
    AttackTime, Decay1Time, Decay2Time, ReleaseTime,     // 0-99
    HoldTime,     // 0-99
    TimeKeyFollow, // 0-7
    Level,        // 0-99
    BreakPoint,   // 0-99 (A-1 = 0)
    LowDepth, HighDepth, // 0-99
    LowCurve, HighCurve, // 0-3: -linear, -exponential, +exponential, +linear
    LevelVelocity, // -7-+7
    PitchDepthOffset, // 0-7
    AmpDepthOffset,   // 0-7
    Wave,         // 0-6: sine, all harmonics 1 and 2, odd harmonics 1 and 2, formant 1 and 2
    WaveSpread,   // 0-7
    WaveFormant,  // 0-99 (formant waves)
    OpCount
};

constexpr int Ops = 8;
constexpr int Count = CommonCount + Ops * OpCount;

// index of an operator value (operator n = 1-8) in Voice::v
constexpr int op(int n, Op k) { return CommonCount + (n - 1) * OpCount + k; }

} // namespace ext

struct ExtendedVoice {
    int16_t v[ext::Count];
    int common(ext::Common k) const { return v[k]; }
    int op(int n, ext::Op k) const { return v[ext::op(n, k)]; }
    bool extended() const { return v[ext::ProfileSel] == ext::Extended; }
};
