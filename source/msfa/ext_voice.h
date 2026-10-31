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
