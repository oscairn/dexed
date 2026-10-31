/*
 * The Extended profile voice: 8 operators, the 88 algorithms, time-based envelopes with a
 * hold segment, per-operator pitch EG (docs/oscairn-plan.md, milestone M4).
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

#include "aligned_buf.h"
#include "ext_voice.h"
#include "fm_op_kernel.h"
#include "synth.h"
#include "synth_rate.h"

class TuningState;
struct Controllers;

// An amplitude envelope in MSFA's log format (Q24, 1 << 24 per doubling), advanced once per
// sample (segments as short as a millisecond keep their measured times): hold (at the
// release level), attack, decay 1, decay 2 (sustained), release.
class ExtEnv {
public:
    // levels and times are the 0-99 panel values; outlevel is the operator's level in the same
    // log format (operator level, scaling and velocity); timeScale stretches every segment
    void init(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
              double sampleRate);
    void update(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                double sampleRate);
    int32_t getsample(); // one sample
    void keydown(bool down);
    bool isActive() const { return ix_ < 5 || levels_[3] > 0; }
    int segment() const { return ix_; }
    void transfer(const ExtEnv &src) { *this = src; }

private:
    void advance(int ix);
    void configure(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                   double sampleRate);

    int levels_[4] = {};
    // levels in Q32 per doubling (the Q24 format with 8 more fraction bits, so the slowest
    // segments' per-sample steps keep their precision)
    int64_t targets_[4] = {};  // per segment
    int64_t inc_[4] = {};      // per segment, per sample
    double attackFrac_ = 0;    // attack: fraction of the way to its asymptote per sample
    int64_t attackOver_ = 0;   // attack: how far above full level its asymptote lies
    int64_t attackStart_ = 0;  // attack: where it starts, below full level
    int64_t full_ = 0;         // the operator's level at EG level 99
    int holdSamples_ = 0;
    int64_t level_ = 0;
    int ix_ = 5;               // 0 hold, 1 attack, 2 decay 1, 3 decay 2, 4 release, 5 done
    int count_ = 0;            // hold: samples left
    bool down_ = false;
};

// The operator pitch EG: from the initial level to the attack level, then to 0, each segment
// straight in cents at its time value's rate (a segment of no distance takes no time), per
// sample.
class ExtPitchEnv {
public:
    void init(int initial, int attack, int attackTime, int decayTime, double sampleRate);
    int32_t getsample(); // log frequency offset (Q24 per octave)
    int32_t level() const { return (int32_t)(level_ >> 16); }
    bool active() const { return ix_ < 2; }
private:
    // Q24 per octave with 16 more fraction bits, so the slowest rates (about 6 Q24 steps a
    // sample) keep their precision
    int64_t level_ = 0, attack_ = 0;
    int64_t incA_ = 1, incD_ = 1; // per sample
    int ix_ = 2;                  // 0 attack, 1 decay, 2 done
};

class ExtNote {
public:
    // an operator's wave (msfa/ext_waves.h)
    struct Wave {
        enum Kind { Sine, Table, Formant, Silent } kind = Sine;
        const int32_t *table = nullptr; // Table: the wave; Formant: the window
        int32_t centre = 1;             // Formant: the centre harmonic
    };
    void init(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning);
    // the voice's values changed while it plays (envelopes keep their state)
    void update(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning);
    // adds to buf; pitchBase is pitch bend and master tune (Q24 log frequency); lfo_samples is
    // the LFO at every sample of the block, or null to use lfo_val throughout
    void compute(int32_t *buf, int32_t lfo_val, const int32_t *lfo_samples, int32_t lfo_delay, int32_t pitchBase,
                 const Controllers *ctrls);
    void keyup();
    bool isPlaying() const;
    void oscSync();            // the operators with Key On Reset start from phase 0
    void transferState(const ExtNote &src);
    void transferSignal(const ExtNote &src);
    void transferPhase(const ExtNote &src);
    void silence() { initialised_ = false; }
    const FmOpParams *params() const { return params_; }

private:
    void setup(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning, bool start);

    bool initialised_ = false;
    int algorithm_ = 0;
    int fbShift_ = 16;
    int64_t fbGain_ = 0;
    uint8_t reset_ = 0;          // operators (FmOpParams index) with Key On Reset
    ExtEnv env_[8];
    ExtPitchEnv peg_[8];
    FmOpParams params_[8] = {};
    int32_t basepitch_[8] = {};
    bool fixed_[8] = {};
    int32_t pitchSens_[8] = {};  // LFO pitch depth offset, as DX7 pitch modulation sensitivity
    int32_t ampSens_[8] = {};
    int32_t pitchDepth_ = 0, ampDepth_ = 0;
    int32_t fbBuf_[2] = {};
    int32_t gains_[8][N];        // per operator, this block's gain per sample
    int32_t freqs_[8][N];        // per operator, this block's phase increment per sample
    Wave waves_[8];
    AlignedBuf<int32_t, N> bufs_[8];
    AlignedBuf<int32_t, N> sum_;
    const SynthRate *rate_ = nullptr;
};
