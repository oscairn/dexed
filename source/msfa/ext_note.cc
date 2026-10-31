/*
 * The Extended profile voice (docs/oscairn-plan.md, milestone M4).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "ext_note.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "controllers.h"
#include "exp2.h"
#include "ext_tables.h"
#include "fm_core.h"
#include "sin.h"
#include "tuning.h"

namespace {

constexpr double kLog2PerDb = 1.0 / 6.020599913279624; // doublings per dB
constexpr double kQ24 = 16777216.0;

int32_t dbToLog(double db) { return (int32_t)std::lround(db * kLog2PerDb * kQ24); }

// Full scale in MSFA's log level: the level Env reaches for a level-99 operator at EG level
// 99 (Env::advance: ((127 >> 1) << 6) + 99 * 32 - 4256 + ... in units of 1 << 16), so an
// Extended carrier at full level is as loud as a Classic one.
constexpr int32_t kFullScale = ((((127 >> 1) << 6) + (127 << 5) - 4256) << 16);

// The modern kernels with sample-by-sample rendering of multi-operator feedback loops.
struct ExtKernels {
    bool gains(FmOpParams &p, int32_t &gain1, int32_t &gain2) {
        const int kLevelThresh = 1120;
        gain1 = p.gain_out;
        gain2 = Exp2::lookup(p.level_in - (14 * (1 << 24)));
        p.gain_out = gain2;
        return gain1 >= kLevelThresh || gain2 >= kLevelThresh;
    }
    void compute(int32_t *o, const int32_t *in, int32_t ph, int32_t f, int32_t g1, int32_t g2, bool add) {
        FmOpKernel::compute(o, in, ph, f, g1, g2, add);
    }
    void compute_pure(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, bool add) {
        FmOpKernel::compute_pure(o, ph, f, g1, g2, add);
    }
    void compute_fb(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *fb, int shift, bool add) {
        FmOpKernel::compute_fb(o, ph, f, g1, g2, fb, shift, add);
    }
    void compute_fb_mod(int32_t *o, const int32_t *in, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *fb,
                        int shift) {
        FmOpKernel::compute_fb_mod(o, in, ph, f, g1, g2, fb, shift);
    }
    int selfFeedbackShift(int shift) { return shift; }
    // A loop feedbackOp > ... > feedbackFrom (algorithms 12 and 14): every operator of the loop
    // sample by sample, the first one taking the average of the last one's two latest outputs
    // (as compute_fb does for itself), the others their modulators' sums.
    bool loop(const CompiledAlgorithm &a, FmOpParams *params, int32_t g1first, int32_t g2first, int32_t *fb,
              int shift, AlignedBuf<int32_t, N> *bufs, bool *audible, uint8_t &rendered) {
        if (a.loopLength < 2)
            return false;
        int32_t g1[8], g2[8], dgain[8], gain[8];
        for (int k = 0; k < a.loopLength; k++) {
            const int op = a.loop[k];
            if (k == 0) {
                g1[k] = g1first;
                g2[k] = g2first;
            } else {
                gains(params[op], g1[k], g2[k]);
            }
            dgain[k] = (g2[k] - g1[k] + (N >> 1)) >> LG_N;
            gain[k] = g1[k];
        }
        int32_t y0 = fb[0], y = fb[1];
        for (int i = 0; i < N; i++) {
            for (int k = 0; k < a.loopLength; k++) {
                const int op = a.loop[k];
                FmOpParams &p = params[op];
                gain[k] += dgain[k];
                int32_t in = 0;
                for (int j = 0; j < a.count; j++)
                    if (((a.inputs[op] >> j) & 1) && audible[j] && j != op)
                        in += bufs[j].get()[i];
                if (k == 0)
                    in = (y0 + y) >> (shift + 1);
                const int32_t ph = p.phase + p.freq * i;
                const int32_t out = (int32_t)(((int64_t)Sin::lookup(ph + in) * gain[k]) >> 24);
                bufs[op].get()[i] = out;
                if (k == a.loopLength - 1) {
                    y0 = y;
                    y = out;
                }
                audible[op] = true; // (within the loop, this sample's output is ready)
            }
        }
        fb[0] = y0;
        fb[1] = y;
        for (int k = 0; k < a.loopLength; k++) {
            const int op = a.loop[k];
            params[op].phase += params[op].freq << LG_N;
            rendered |= (uint8_t)(1u << op);
        }
        return true;
    }
};

} // namespace

// ---- envelope ----

int32_t ExtEnv::target(int i) const { return targets_[i]; }

void ExtEnv::configure(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                       double sampleRate) {
    const double blocksPerSecond = sampleRate / N;
    for (int i = 0; i < 4; i++) {
        levels_[i] = std::clamp(levels[i], 0, 99);
        const double db = extEgLevelDb(levels_[i]);
        targets_[i] = std::max(0, outlevel + dbToLog(db));
        // segment i (attack, decay 1, decay 2, release): dB per block
        const double msPerDb = (i == 0 ? extAttackMsPerDb(std::clamp(times[i], 0, 99))
                                       : extEgMsPerDb(std::clamp(times[i], 0, 99))) * timeScale;
        const double dbPerBlock = 1000.0 / (msPerDb * blocksPerSecond);
        inc_[i] = std::max(1, dbToLog(dbPerBlock));
        if (i == 0) {
            // the attack approaches a point extAttackOverDb above its target exponentially
            // in dB, at the segment's rate in doublings per second (see ext_tables.h)
            const double perBlock = 1.0 - std::exp(-dbPerBlock / 6.020599913279624);
            attackFrac_ = (int32_t)std::lround(perBlock * (1 << 30));
            attackOver_ = dbToLog(extAttackOverDb());
            attackStart_ = dbToLog(-extAttackStartDb());
        }
    }
    holdBlocks_ = (int)std::lround(extHoldMs(std::clamp(holdTime, 0, 99)) * timeScale * blocksPerSecond / 1000.0);
}

void ExtEnv::init(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                  double sampleRate) {
    configure(levels, times, holdTime, outlevel, timeScale, sampleRate);
    down_ = true;
    level_ = targets_[3]; // the hold level
    count_ = holdBlocks_;
    ix_ = count_ > 0 ? 0 : 1;
}

void ExtEnv::update(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                    double sampleRate) {
    configure(levels, times, holdTime, outlevel, timeScale, sampleRate);
}

void ExtEnv::advance(int ix) { ix_ = ix; }

int32_t ExtEnv::getsample() {
    switch (ix_) {
    case 0:
        if (--count_ <= 0)
            ix_ = 1;
        return level_;
    case 1: case 2: case 3: case 4: {
        const int seg = ix_ - 1;
        const int32_t t = targets_[seg];
        if (seg == 0 && level_ < t) {
            level_ = std::max(level_, t + attackStart_);
            const int64_t step = ((int64_t)(t + attackOver_ - level_) * attackFrac_) >> 30;
            level_ = std::min(t, level_ + (int32_t)std::max<int64_t>(step, 1));
        } else if (level_ < t) {
            level_ = std::min(t, level_ + inc_[seg]);
        } else if (level_ > t) {
            level_ = std::max(t, level_ - inc_[seg]);
        }
        if (level_ == t && ix_ < 3)
            ix_++;           // decay 2 holds its level while the key is down
        else if (level_ == t && ix_ == 4)
            ix_ = 5;
        return level_;
    }
    default:
        return level_;
    }
}

void ExtEnv::keydown(bool down) {
    if (down_ == down)
        return;
    down_ = down;
    if (!down)
        ix_ = 4;
}

// ---- operator pitch EG ----

void ExtPitchEnv::init(int initial, int attack, int attackTime, int decayTime, double sampleRate) {
    const double blocksPerSecond = sampleRate / N;
    level_ = (int32_t)std::lround(extPegCents(initial) / 1200.0 * kQ24);
    attack_ = (int32_t)std::lround(extPegCents(attack) / 1200.0 * kQ24);
    // octaves per block over the full range of the EG (from -50 to +50)
    const double span = (extPegCents(50) - extPegCents(-50)) / 1200.0 * kQ24;
    incA_ = std::max(1, (int)std::lround(span / (extPegMs(attackTime) * blocksPerSecond / 1000.0)));
    incD_ = std::max(1, (int)std::lround(span / (extPegMs(decayTime) * blocksPerSecond / 1000.0)));
    ix_ = level_ == 0 && attack_ == 0 ? 2 : 0;
}

int32_t ExtPitchEnv::getsample() {
    if (ix_ < 2) {
        const int32_t t = ix_ == 0 ? attack_ : 0;
        const int32_t inc = ix_ == 0 ? incA_ : incD_;
        if (level_ < t)
            level_ = std::min(t, level_ + inc);
        else if (level_ > t)
            level_ = std::max(t, level_ - inc);
        if (level_ == t)
            ix_++;
    }
    return level_;
}

// ---- voice ----

void ExtNote::setup(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning,
                    bool start) {
    rate_ = rate;
    const double sampleRate = rate->sampleRate;
    algorithm_ = std::clamp(v.common(ext::Algorithm), 1, 88) - 1;
    const int feedback = std::clamp(v.common(ext::Feedback), 0, 7);
    fbShift_ = feedback != 0 ? 8 - feedback : 16;
    pitchDepth_ = (std::clamp(v.common(ext::LfoPitchDepth), 0, 99) * 165) >> 6;
    ampDepth_ = (std::clamp(v.common(ext::LfoAmpDepth), 0, 99) * 165) >> 6;
    const int32_t notePitch = tuning.midinote_to_logfreq(midinote);
    reset_ = 0;
    for (int n = 1; n <= ext::Ops; n++) {
        const int i = ext::Ops - n; // FmOpParams index (compileAlgorithm's order)
        auto o = [&](ext::Op k) { return v.op(n, k); };
        if (o(ext::KeyOnReset))
            reset_ |= (uint8_t)(1u << i);
        // frequency
        fixed_[i] = o(ext::Mode) != 0;
        double cents = extPitchVelocityCents(o(ext::PitchVelocity), velocity);
        int32_t pitch;
        if (fixed_[i]) {
            cents += extKeyFollowCents(o(ext::KeyFollow), midinote);
            pitch = (int32_t)std::lround(std::log2(extFixedHz(o(ext::Coarse), o(ext::Fine))) * kQ24);
        } else {
            cents += extDetuneCents(o(ext::Detune), midinote);
            pitch = notePitch + (int32_t)std::lround(std::log2(extRatio(o(ext::Coarse), o(ext::Fine))) * kQ24);
        }
        basepitch_[i] = pitch + (int32_t)std::lround(cents / 1200.0 * kQ24);
        // level: operator level, scaling and velocity, in dB, then MSFA's log format
        double db = extOpLevelDb(std::clamp(o(ext::Level), 0, 99));
        db += extLevelScalingDb(midinote, o(ext::BreakPoint), o(ext::LowDepth), o(ext::HighDepth), o(ext::LowCurve),
                                o(ext::HighCurve));
        db += extLevelVelocityDb(o(ext::LevelVelocity), velocity);
        const int32_t outlevel = kFullScale + dbToLog(std::min(0.0, db));
        const int levels[4] = { o(ext::AttackLevel), o(ext::Decay1Level), o(ext::Decay2Level), o(ext::ReleaseLevel) };
        const int times[4] = { o(ext::AttackTime), o(ext::Decay1Time), o(ext::Decay2Time), o(ext::ReleaseTime) };
        const double timeScale = extTimeKeyFollowScale(o(ext::TimeKeyFollow), midinote);
        if (start)
            env_[i].init(levels, times, o(ext::HoldTime), outlevel, timeScale, sampleRate);
        else
            env_[i].update(levels, times, o(ext::HoldTime), outlevel, timeScale, sampleRate);
        if (start)
            peg_[i].init(o(ext::PegInitial), o(ext::PegAttack), o(ext::PegAttackTime), o(ext::PegDecayTime), sampleRate);
        pitchSens_[i] = extPitchDepthOffset(std::clamp(o(ext::PitchDepthOffset), 0, 7));
        ampSens_[i] = extAmpDepthOffset(std::clamp(o(ext::AmpDepthOffset), 0, 7));
    }
}

void ExtNote::init(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning) {
    initialised_ = true;
    setup(v, midinote, velocity, rate, tuning, true);
}

void ExtNote::update(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning) {
    setup(v, midinote, velocity, rate, tuning, false);
}

void ExtNote::compute(int32_t *buf, int32_t lfo_val, int32_t lfo_delay, int32_t pitchBase, const Controllers *ctrls) {
    // LFO pitch and amplitude modulation as the DX7 voice computes them, per operator
    const uint32_t pmd = (uint32_t)pitchDepth_ * (uint32_t)lfo_delay; // Q32
    const int32_t lfoBipolar = lfo_val - (1 << 23);
    const int32_t lfoInv = (1 << 24) - lfo_val;
    uint32_t amd = (uint32_t)(((int64_t)ampDepth_ * (int64_t)lfo_delay) >> 8);
    amd = (uint32_t)(((int64_t)amd * (int64_t)lfoInv) >> 24);
    const uint32_t amodWheel = (uint32_t)(((int64_t)ctrls->amp_mod * (int64_t)lfoInv) >> 7);
    amd = std::max(amd, amodWheel);
    for (int i = 0; i < ext::Ops; i++) {
        const int32_t senslfo = pitchSens_[i] * lfoBipolar;
        int32_t pmod = (int32_t)(((int64_t)pmd * (int64_t)senslfo) >> 39);
        const int32_t pmodWheel = (int32_t)(((int64_t)ctrls->pitch_mod * (int64_t)senslfo) >> 14);
        pmod = std::abs(pmod) >= std::abs(pmodWheel) ? pmod : pmodWheel;
        int32_t pitch = basepitch_[i] + peg_[i].getsample() + pitchBase;
        if (!fixed_[i])
            pitch += pmod;
        params_[i].freq = rate_->freqlut.lookup(pitch);
        int32_t level = env_[i].getsample();
        if (ampSens_[i] != 0) {
            const uint32_t sensamp = (uint32_t)(((uint64_t)amd * (uint64_t)ampSens_[i]) >> 24);
            const uint32_t pt = (uint32_t)std::exp(((float)sensamp) / 262144 * 0.07 + 12.2);
            const uint32_t ldiff = (uint32_t)(((uint64_t)level * ((uint64_t)pt << 4)) >> 28);
            level -= (int32_t)ldiff;
        }
        params_[i].level_in = level;
    }
    ExtKernels k;
    renderGraph(k, extCompiled(algorithm_), buf, params_, fbBuf_, fbShift_, bufs_, sum_.get());
}

void ExtNote::keyup() {
    for (auto &e : env_)
        e.keydown(false);
}

bool ExtNote::isPlaying() const {
    if (!initialised_)
        return false;
    const CompiledAlgorithm &a = extCompiled(algorithm_);
    for (int i = 0; i < ext::Ops; i++)
        if (((a.outputs >> i) & 1) && env_[i].isActive())
            return true;
    return false;
}

void ExtNote::oscSync() {
    for (int i = 0; i < ext::Ops; i++)
        if ((reset_ >> i) & 1) {
            params_[i].phase = 0;
            params_[i].gain_out = 0;
        }
    fbBuf_[0] = fbBuf_[1] = 0;
}

void ExtNote::transferState(const ExtNote &src) {
    for (int i = 0; i < ext::Ops; i++) {
        env_[i].transfer(src.env_[i]);
        params_[i].gain_out = src.params_[i].gain_out;
        params_[i].phase = src.params_[i].phase;
    }
}

void ExtNote::transferSignal(const ExtNote &src) {
    for (int i = 0; i < ext::Ops; i++) {
        params_[i].gain_out = src.params_[i].gain_out;
        params_[i].phase = src.params_[i].phase;
    }
}

void ExtNote::transferPhase(const ExtNote &src) {
    for (int i = 0; i < ext::Ops; i++)
        params_[i].phase = src.params_[i].phase;
}
