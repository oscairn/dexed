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
    // the part LFO (msfa/ext_part_lfo.h)
    PartLfoPhase,       // 0-5: 0, 90, 120, 180, 240, 270 degrees
    PartLfoWave,        // 0-12 (ExtPartLfo::Wave)
    PartLfoSpeed,       // 0-63
    PartLfoSync,        // 0-1: tempo sync
    PartLfoTempoSpeed,  // 0-19: 16th ... 4th x 64 (extPartLfoTempoBeats)
    PartLfoDelay,       // 0-127
    PartLfoFadeIn,      // 0-127
    PartLfoHold,        // 0-127 (127: hold for ever)
    PartLfoFadeOut,     // 0-127
    PartLfoKeyReset,    // 0 off, 1 each key-on, 2 the first key-on
    PartLfoDest1, PartLfoDepth1, PartLfoDest2, PartLfoDepth2, PartLfoDest3, PartLfoDepth3, // destination 0-9 (extPartLfoDest), depth 0-127
    PartLfoUserCycle,   // 0-6: 2, 3, 4, 6, 8, 12, 16 steps
    PartLfoUserSlope,   // 0-3: off, up, down, up and down
    PartLfoStep1, PartLfoStep2, PartLfoStep3, PartLfoStep4, PartLfoStep5, PartLfoStep6, PartLfoStep7, PartLfoStep8,
    PartLfoStep9, PartLfoStep10, PartLfoStep11, PartLfoStep12, PartLfoStep13, PartLfoStep14, PartLfoStep15,
    PartLfoStep16,      // -64-+63
    PartLfoRandomSpeed, // 0-127
    // part values that act on the sound (msfa/ext_tables.h extPartVelocity, extAegOffsetSteps)
    VelocityDepth, VelocityOffset, // 0-127 (64 and 64 leave the velocity unchanged)
    AegAttackOffset, AegDecayOffset, AegSustainOffset, AegReleaseOffset, // -64-+63: every operator's EG
    FegDepthOffset, CutoffOffset, ResonanceOffset, // -64-+63
    NoteShift,          // -48-+48 semitones
    PartDetune,         // -128-+127 (documented as tenths of a Hz; plays cents, extPartDetuneCents)
    // the macros: offsets to the modulators' values (ExtNote::setup)
    MacroDepth, MacroRatio, MacroAttack, MacroDecay, MacroSustain, MacroRelease, MacroTexture, // -99-+99
    BendUpper, BendLower, // -48-+24 semitones: the pitch at full bend up and full bend down
    // how the sound plays its notes (SynthEngine)
    MonoPoly,           // 0 mono, 1 poly
    KeyAssign,          // 0 single, 1 multi: a key played again while it sounds
    Portamento,         // 0-1
    PortamentoTime,     // 0-127
    PortamentoMode,     // 0 fingered (legato notes only), 1 full-time
    PortamentoTimeMode, // 0-3: rate 1, time 1, rate 2, time 2
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
    PartLfoBox1, PartLfoBox2, PartLfoBox3,       // 0-1: the part LFO's destinations 1-3 reach this operator
    PartLfoRatio1, PartLfoRatio2, PartLfoRatio3, // 0-127: their depth on this operator
    OpCount
};

// part LFO destinations (PartLfoDest1-3)
enum PartLfoDest { PartLfoNone, PartLfoPan, PartLfoLfo2Speed, PartLfoCutoff, PartLfoResonance, PartLfoFeedback,
                   PartLfoOpFrequency, PartLfoOpWave, PartLfoOpDetune, PartLfoOpLevel };

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
