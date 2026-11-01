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
#include "ext_filter.h"
#include "ext_voice.h"
#include "fm_op_kernel.h"
#include "lfo.h"
#include "synth.h"
#include "synth_rate.h"

class TuningState;
class Controllers;

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

// The common pitch EG (every operator): from the initial level to the attack, decay 1 and
// decay 2 levels (held while the key is down), then from wherever it is to the release level;
// each segment straight in cents at its time value's rate (a segment of no distance takes no
// time), per sample.
class ExtPartPitchEnv {
public:
    // levels in cents (initial, attack, decay 1, decay 2, release); rates in cents per ms
    // (attack, decay 1, decay 2, release)
    void init(const double cents[5], const double centsPerMs[4], double sampleRate);
    int32_t getsample(); // log frequency offset (Q24 per octave)
    int32_t level() const { return (int32_t)(level_ >> 16); }
    bool active() const { return ix_ < 4; }
    void keyup();
private:
    int64_t level_ = 0;      // Q24 per octave with 16 more fraction bits
    int64_t target_[4] = {}; // attack, decay 1, decay 2, release
    int64_t inc_[4] = { 1, 1, 1, 1 };
    int ix_ = 4;             // 0 attack, 1 decay 1, 2 decay 2, 3 release, 4 still
};

// The filter EG: levels in cents (hold, attack, decay 1, decay 2 sustained, release). Each
// segment takes its time value's duration whatever its distance (measured), moving straight in
// cents; advanced per block.
class ExtFilterEnv {
public:
    // levels -128-+127 and times 0-127 as the panel shows them (hold, attack, decay 1, decay 2,
    // release); timeScale stretches every segment
    void init(const int levels[5], const int times[5], double timeScale, double sampleRate);
    void update(const int levels[5], const int times[5], double timeScale, double sampleRate);
    double getblock(int n); // advances n samples; the level in cents at the block's end
    void keydown(bool down);
    void transfer(const ExtFilterEnv &src) { *this = src; }
private:
    void configure(const int levels[5], const int times[5], double timeScale, double sampleRate);
    void enter(int ix);
    double target_[5] = {};  // cents: hold, attack, decay 1, decay 2, release
    double samples_[5] = {}; // each segment's duration (hold: its length)
    double level_ = 0;
    double step_ = 0;        // this segment's cents per sample
    int ix_ = 5;             // 0 hold, 1 attack, 2 decay 1, 3 decay 2, 4 release, 5 done
    double left_ = 0;        // samples left in this segment
    bool down_ = false;
};

// What a note takes from its sound's state at note-on.
struct ExtStart {
    uint32_t randomPitch = 0, randomPan = 0; // uniform 32-bit values drawn for this note
    uint32_t count = 0;                      // the sound's Extended notes before this one (alternate pan)
    double tempo = 120;                      // beats per minute (tempo-synced key-on delay)
    int64_t wait = 0;                        // samples before the note starts, added to its key-on delay
    double velocity = 0;                     // the velocity with its fraction (the part's sensitivity), or 0
    uint32_t lfoPhase = 0;                   // the sound's free-running 2nd LFO phase (no key-on reset)
};

class ExtNote {
public:
    // an operator's wave (msfa/ext_waves.h)
    struct Wave {
        enum Kind { Sine, Table, Formant, Silent } kind = Sine;
        const int32_t *table = nullptr; // Table: the wave; Formant: the window
        int32_t centre = 1;             // Formant: the centre harmonic
    };
    void init(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning,
              const ExtStart &start = ExtStart());
    // how long a sounding note of this sound takes to fade out when a new note takes its place
    // without portamento (seconds)
    static double retriggerFade(const ExtendedVoice &v);
    // its pan gains (both 1 at the centre)
    float gainLeft() const { return gainL_; }
    float gainRight() const { return gainR_; }
    // the voice's values changed while it plays (envelopes keep their state)
    void update(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning);
    // adds to buf; pitchBase is pitch bend and master tune (Q24 log frequency); part_lfo: the
    // sound's part LFO at every sample of the block (-1..+1), or null; lfoSpeedSteps: the part
    // LFO's move of the 2nd LFO's speed this block
    void compute(int32_t *buf, int32_t pitchBase, const Controllers *ctrls, const float *part_lfo = nullptr,
                 int lfoSpeedSteps = 0);
    bool waiting() const { return delay_ > 0; } // in its key-on delay
    void keyup();
    bool isPlaying() const;
    void oscSync();            // the operators with Key On Reset start from phase 0
    void transferState(const ExtNote &src);
    void transferSignal(const ExtNote &src);
    void transferPhase(const ExtNote &src);
    // portamento: glide from `from` (Q24 log frequency of the note's key, see pitch()) to this
    // note's key at `semitonesPerSecond`; ratio-mode operators only
    void glideFrom(double from, double semitonesPerSecond);
    double pitch() const { return notePitch_ + glide_; } // the key's pitch now, Q24 log frequency
    void silence() { initialised_ = false; }
    const FmOpParams *params() const { return params_; }

private:
    void setup(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning, bool start);
    void setFeedback(int feedback); // fbShift_ and fbGain_ for feedback 0-7
    void chooseWave(int i, int wave); // operator i's wave table for wave 0-6
    void render(int32_t *buf, int32_t pitchBase, const Controllers *ctrls, const float *part_lfo);
    void setupFilter(const ExtendedVoice &v, int midinote, int velocity, bool start);
    // partLfo: the part LFO this block (-1..+1)
    void filterBlock(int32_t *buf, const int32_t *lfo, int32_t lfo_delay, double partLfo);

    bool initialised_ = false;
    int algorithm_ = 0;
    int fbShift_ = 16;
    int64_t fbGain_ = 0;
    uint8_t reset_ = 0;          // operators (FmOpParams index) with Key On Reset
    ExtEnv env_[8];
    ExtPitchEnv peg_[8];
    ExtPartPitchEnv ppeg_;
    uint32_t random_ = 0;        // this note's random value (random pitch)
    float gainL_ = 1, gainR_ = 1; // pan
    int panPos_ = 0;              // the note's own pan position
    double partLfoPan_ = 0;       // part LFO to pan: positions at the LFO's top
    // key-on delay: samples before the note starts, then its output runs `shift_` samples late
    // within the 64-sample grid (carry_ holds the samples that spill into the next block)
    int64_t delay_ = 0;
    int shift_ = 0;
    double velocity_ = 0;        // ExtStart::velocity (operator level velocity)
    double notePitch_ = 0;       // the key's log frequency (Q24)
    double glide_ = 0;           // portamento: offset from it, Q24, moving to 0 by glideStep_ a sample
    double glideStep_ = 0;
    int32_t carry_[N] = {};
    FmOpParams params_[8] = {};
    int32_t basepitch_[8] = {};
    bool fixed_[8] = {};
    int32_t pitchSens_[8] = {};  // LFO pitch depth offset, as DX7 pitch modulation sensitivity
    float partLfoPitch_[8] = {}; // part LFO to this operator's frequency: Q24 log frequency at +1
    float partLfoLevel_[8] = {}; // part LFO to this operator's level: log level lost at -1
    double partLfoDetune_[8] = {}; // part LFO to this operator's detune: steps at +1
    int detune_[8] = {};         // each ratio-mode operator's detune
    int detuneNote_ = 60;
    int feedback_ = 0;           // the sound's feedback level
    int wave_[8] = {}, waveNow_[8] = {}, spread_[8] = {}, formant_[8] = {}, below_[8] = {};
    double partLfoWave_[8] = {}; // part LFO to this operator's wave: wave steps at +1
    double partLfoFeedback_ = 0; // part LFO to feedback: steps at +1
    int32_t ampSens_[8] = {};
    int32_t pitchDepth_ = 0, ampDepth_ = 0;
    Lfo lfo_;                    // this voice's 2nd LFO (per voice in the reference)
    int32_t fbBuf_[2] = {};
    int32_t gains_[8][N];        // per operator, this block's gain per sample
    int32_t freqs_[8][N];        // per operator, this block's phase increment per sample
    Wave waves_[8];
    AlignedBuf<int32_t, N> bufs_[8];
    AlignedBuf<int32_t, N> sum_;
    const SynthRate *rate_ = nullptr;

    // the filter (msfa/ext_filter.h): what stays fixed while the note plays
    struct FilterState {
        int type = extfilter::Thru;
        double value = 1023;     // the cutoff value with scaling
        double ratio = 1;        // key follow and velocity, as a frequency ratio
        double q = 0.5;
        double ratio2 = 1;       // dual types: Distance; LPF18s
        double widthHigh = 0, widthLow = 0; // BPFw: the edges, in cutoff values from the cutoff
        double hpf = 20;         // Hz
        double gain = 1;
        double fegOctaves = 0;   // FEG depth: octaves per cent of FEG level (0: no FEG)
        double lfoOctaves = 0;   // LFO filter depth: octaves at the LFO's peak
        double partLfoValues = 0; // part LFO cutoff destination: cutoff values at the LFO's top
        int resonance = 0;       // the resonance value, and resonance velocity's factor on Q
        double qVelocity = 1;
        double partLfoRes = 0;   // part LFO resonance destination: resonance steps at the LFO's top
        bool active = false;     // false: Thru at unity gain (the operators' sum goes out unchanged)
    } filt_;
    ExtFilter filter_;
    ExtFilterEnv feg_;
    bool filterStarted_ = false;
};
