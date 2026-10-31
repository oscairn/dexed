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
#include "ext_waves.h"
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
    // each operator's per-sample gains for this block (ExtNote::compute), indexed by its
    // FmOpParams index; `cur` is the operator renderGraph asked gains() about last
    const int32_t (*gainOf)[N] = nullptr;
    // each operator's phase increment per sample (the LFO moves pitch per sample); the `f`
    // arguments below are ignored
    const int32_t (*freqOf)[N] = nullptr;
    const FmOpParams *base = nullptr;
    int64_t fbGain = 0; // feedback: the average of the last two outputs times this, Q24
    const ExtNote::Wave *waves = nullptr; // each operator's wave
    int cur = 0;
    int32_t wave(int op, int32_t ph) const {
        const ExtNote::Wave &w = waves[op];
        switch (w.kind) {
        case ExtNote::Wave::Table:
            return extwave::lookup(w.table, ph);
        case ExtNote::Wave::Formant:
            return (int32_t)(((int64_t)extwave::lookup(w.table, ph) * Sin::lookup(ph * w.centre)) >> 24);
        case ExtNote::Wave::Silent:
            return 0;
        default:
            return Sin::lookup(ph);
        }
    }
    bool gains(FmOpParams &p, int32_t &gain1, int32_t &gain2) {
        const int kLevelThresh = 1120;
        cur = (int)(&p - base);
        const int32_t *g = gainOf[cur];
        gain1 = g[0];
        gain2 = g[N - 1];
        p.gain_out = gain2;
        int32_t most = 0;
        for (int i = 0; i < N; i++)
            most = std::max(most, g[i]);
        return most >= kLevelThresh;
    }
    void ramp(int32_t, int32_t, int32_t *g) const { std::copy(gainOf[cur], gainOf[cur] + N, g); }
    void compute(int32_t *o, const int32_t *in, int32_t ph, int32_t, int32_t g1, int32_t g2, bool add) {
        int32_t g[N];
        ramp(g1, g2, g);
        const int32_t *f = freqOf[cur];
        for (int i = 0; i < N; i++, ph += f[i - 1]) {
            const int32_t y = (int32_t)(((int64_t)wave(cur, ph + in[i]) * g[i]) >> 24);
            o[i] = add ? o[i] + y : y;
        }
    }
    void compute_pure(int32_t *o, int32_t ph, int32_t, int32_t g1, int32_t g2, bool add) {
        int32_t g[N];
        ramp(g1, g2, g);
        const int32_t *f = freqOf[cur];
        for (int i = 0; i < N; i++, ph += f[i - 1]) {
            const int32_t y = (int32_t)(((int64_t)wave(cur, ph) * g[i]) >> 24);
            o[i] = add ? o[i] + y : y;
        }
    }
    void compute_fb_mod(int32_t *o, const int32_t *in, int32_t ph, int32_t, int32_t g1, int32_t g2, int32_t *fb,
                        int shift) {
        int32_t g[N];
        ramp(g1, g2, g);
        const int32_t *f = freqOf[cur];
        int32_t y0 = fb[0], y = fb[1];
        for (int i = 0; i < N; i++, ph += f[i - 1]) {
            const int32_t scaled = (int32_t)(((int64_t)(y0 + y) * fbGain) >> 25);
            y0 = y;
            y = (int32_t)(((int64_t)wave(cur, ph + (in ? in[i] : 0) + scaled) * g[i]) >> 24);
            o[i] = y;
        }
        fb[0] = y0;
        fb[1] = y;
    }
    void compute_fb(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *fb, int shift, bool add) {
        if (!add) {
            compute_fb_mod(o, nullptr, ph, f, g1, g2, fb, shift);
            return;
        }
        int32_t tmp[N];
        compute_fb_mod(tmp, nullptr, ph, f, g1, g2, fb, shift);
        for (int i = 0; i < N; i++)
            o[i] += tmp[i];
    }
    int selfFeedbackShift(int shift) { return shift; }
    // A loop feedbackOp > ... > feedbackFrom (algorithms 12 and 14): every operator of the loop
    // sample by sample, each from its modulators' sum, the first one also taking the average
    // of the last one's two latest outputs (as compute_fb does for itself).
    bool loop(const CompiledAlgorithm &a, FmOpParams *params, int32_t g1first, int32_t g2first, int32_t *fb,
              int shift, AlignedBuf<int32_t, N> *bufs, bool *audible, uint8_t &rendered) {
        if (a.loopLength < 2)
            return false;
        (void)g1first;
        (void)g2first;
        for (int k = 1; k < a.loopLength; k++) {
            int32_t unused1, unused2;
            gains(params[a.loop[k]], unused1, unused2); // (updates gain_out)
        }
        int32_t y0 = fb[0], y = fb[1];
        int32_t ph[8];
        for (int k = 0; k < a.loopLength; k++)
            ph[k] = params[a.loop[k]].phase;
        for (int i = 0; i < N; i++) {
            for (int k = 0; k < a.loopLength; k++) {
                const int op = a.loop[k];
                const int32_t gain = gainOf[op][i];
                int32_t in = 0;
                for (int j = 0; j < a.count; j++)
                    if (((a.inputs[op] >> j) & 1) && audible[j] && j != op)
                        in += bufs[j].get()[i];
                if (k == 0)
                    in += (int32_t)(((int64_t)(y0 + y) * fbGain) >> 25);
                const int32_t out = (int32_t)(((int64_t)wave(op, ph[k] + in) * gain) >> 24);
                ph[k] += freqOf[op][i];
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

void ExtEnv::configure(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                       double sampleRate) {
    constexpr double q32 = 4294967296.0;
    full_ = (int64_t)outlevel << 8;
    for (int i = 0; i < 4; i++) {
        levels_[i] = std::clamp(levels[i], 0, 99);
        const double db = extEgLevelDb(levels_[i]);
        targets_[i] = std::max<int64_t>(0, ((int64_t)outlevel << 8) + (int64_t)std::llround(db * kLog2PerDb * q32));
        // segment i (attack, decay 1, decay 2, release): dB per sample
        const double msPerDb = (i == 0 ? extAttackMsPerDb(std::clamp(times[i], 0, 99))
                                       : extEgMsPerDb(std::clamp(times[i], 0, 99))) * timeScale;
        const double dbPerSample = 1000.0 / (msPerDb * sampleRate);
        inc_[i] = std::max<int64_t>(1, std::llround(dbPerSample * kLog2PerDb * q32));
        if (i == 0) {
            // the attack starts extAttackStartDb below the operator's full level and approaches
            // a point extAttackOverDb above full level exponentially in dB (as the DX7's
            // does), at the segment's rate in doublings per second; it stops at its target
            attackFrac_ = 1.0 - std::exp(-dbPerSample * kLog2PerDb);
            attackOver_ = std::llround(extAttackOverDb() * kLog2PerDb * q32);
            attackStart_ = std::llround(-extAttackStartDb() * kLog2PerDb * q32);
        }
    }
    holdSamples_ = (int)std::lround(extHoldMs(std::clamp(holdTime, 0, 99)) * timeScale * sampleRate / 1000.0);
}

void ExtEnv::init(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                  double sampleRate) {
    configure(levels, times, holdTime, outlevel, timeScale, sampleRate);
    down_ = true;
    level_ = targets_[3]; // the hold level
    count_ = holdSamples_;
    ix_ = count_ > 0 ? 0 : 1;
}

void ExtEnv::update(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                    double sampleRate) {
    configure(levels, times, holdTime, outlevel, timeScale, sampleRate);
}

void ExtEnv::advance(int ix) { ix_ = ix; }

int32_t ExtEnv::getsample() {
    if (ix_ == 0) {
        if (--count_ <= 0)
            ix_ = 1;
    } else if (ix_ <= 4) {
        const int seg = ix_ - 1;
        const int64_t t = targets_[seg];
        if (seg == 0 && level_ < t) {
            level_ = std::max(level_, full_ + attackStart_);
            const int64_t step = (int64_t)((double)(full_ + attackOver_ - level_) * attackFrac_);
            level_ = std::min(t, level_ + std::max<int64_t>(step, 1));
        } else if (level_ < t) {
            level_ = std::min(t, level_ + inc_[seg]);
        } else if (level_ > t) {
            level_ = std::max(t, level_ - inc_[seg]);
        }
        if (level_ == t && ix_ < 3)
            ix_++;           // decay 2 holds its level while the key is down
        else if (level_ == t && ix_ == 4)
            ix_ = 5;
    }
    return (int32_t)(level_ >> 8);
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
    level_ = (int64_t)std::lround(extPegCents(initial) / 1200.0 * kQ24) << 16;
    attack_ = (int64_t)std::lround(extPegCents(attack) / 1200.0 * kQ24) << 16;
    auto inc = [&](int t) {
        return std::max<int64_t>(1, std::llround(extPegCentsPerMs(t) / 1200.0 * kQ24 * 65536.0 * 1000.0 / sampleRate));
    };
    incA_ = inc(attackTime);
    incD_ = inc(decayTime);
    ix_ = level_ == 0 && attack_ == 0 ? 2 : 0;
}

int32_t ExtPitchEnv::getsample() {
    if (ix_ < 2) {
        const int64_t t = ix_ == 0 ? attack_ : 0;
        const int64_t inc = ix_ == 0 ? incA_ : incD_;
        if (level_ < t)
            level_ = std::min(t, level_ + inc);
        else if (level_ > t)
            level_ = std::max(t, level_ - inc);
        if (level_ == t)
            ix_++;
    }
    return level();
}

// ---- voice ----

void ExtNote::setup(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning,
                    bool start) {
    rate_ = rate;
    const double sampleRate = rate->sampleRate;
    algorithm_ = std::clamp(v.common(ext::Algorithm), 1, 88) - 1;
    const int feedback = std::clamp(v.common(ext::Feedback), 0, 7);
    fbShift_ = feedback != 0 ? 8 - feedback : 16; // (16: off, for renderGraph)
    // the measured feedback amount, relative to the DX7's shift for the same value
    fbGain_ = feedback != 0 ? std::llround(extFeedbackScale(feedback) * (double)(1 << 24) / (1 << fbShift_)) : 0;
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
        double cents = extPitchVelocityCents(v.common(ext::PitchVelocityDepth), o(ext::PitchVelocity), velocity);
        int32_t pitch;
        if (fixed_[i]) {
            cents += extKeyFollowCents(o(ext::KeyFollow), midinote);
            pitch = (int32_t)std::lround(std::log2(extFixedHz(o(ext::Coarse), o(ext::Fine))) * kQ24);
        } else {
            cents += extDetuneCents(o(ext::Detune), midinote);
            pitch = notePitch + (int32_t)std::lround(std::log2(extRatio(o(ext::Coarse), o(ext::Fine))) * kQ24);
        }
        basepitch_[i] = pitch + (int32_t)std::lround(cents / 1200.0 * kQ24);
        // the wave, band-limited for this note's frequency (shortcut: chosen at note-on and
        // on edits; pitch EG, LFO and bend moves are not followed)
        {
            const double hz = rate_->freqlut.lookup(basepitch_[i]) * sampleRate / kQ24;
            const int below = (int)std::floor(sampleRate / 2 / std::max(hz, 1.0)); // harmonics below Nyquist
            const int w = std::clamp(o(ext::Wave), 0, 6), spread = std::clamp(o(ext::WaveSpread), 0, 7);
            Wave &wv = waves_[i];
            if (w == 0) {
                wv = Wave {};
            } else if (w <= 4) {
                wv.kind = Wave::Table;
                wv.table = extwave::table(w, spread, below);
            } else {
                wv.centre = std::clamp(o(ext::WaveFormant), 0, 99) + 1;
                wv.table = extwave::window(w, spread, below - wv.centre);
                wv.kind = wv.table ? Wave::Formant : Wave::Silent;
            }
        }
        // level: operator level, scaling and velocity, in dB, then MSFA's log format
        // modulators have their own level curve (the modulation index the reference plays)
        const bool modulator = !((extCompiled(algorithm_).outputs >> i) & 1);
        double db = modulator ? extModLevelDb(std::clamp(o(ext::Level), 0, 99))
                              : extOpLevelDb(std::clamp(o(ext::Level), 0, 99));
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

void ExtNote::compute(int32_t *buf, int32_t lfo_val, const int32_t *lfo_samples, int32_t lfo_delay,
                      int32_t pitchBase, const Controllers *ctrls) {
    // LFO pitch and amplitude modulation as the DX7 voice computes them, per operator, but at
    // every sample when the LFO runs per sample
    int32_t lfo[N];
    for (int s = 0; s < N; s++)
        lfo[s] = lfo_samples ? lfo_samples[s] : lfo_val;
    const uint32_t pmd = (uint32_t)pitchDepth_ * (uint32_t)lfo_delay; // Q32
    const bool pitchMod = pmd != 0 || ctrls->pitch_mod != 0;
    const uint32_t amdDepth = (uint32_t)(((int64_t)ampDepth_ * (int64_t)lfo_delay) >> 8);
    uint32_t amd[N];
    bool ampMod = false;
    for (int s = 0; s < N; s++) {
        const int32_t lfoInv = (1 << 24) - lfo[s];
        const uint32_t a = (uint32_t)(((int64_t)amdDepth * (int64_t)lfoInv) >> 24);
        const uint32_t w = (uint32_t)(((int64_t)ctrls->amp_mod * (int64_t)lfoInv) >> 7);
        amd[s] = std::max(a, w);
        ampMod |= amd[s] != 0;
    }
    uint32_t phase0[ext::Ops];
    for (int i = 0; i < ext::Ops; i++) {
        const int32_t pitch = basepitch_[i] + pitchBase;
        int32_t *f = freqs_[i];
        const bool lfoPitch = !fixed_[i] && pitchSens_[i] != 0 && pitchMod;
        if (lfoPitch || peg_[i].active()) {
            for (int s = 0; s < N; s++) {
                int32_t pmod = 0;
                if (lfoPitch) {
                    const int32_t senslfo = pitchSens_[i] * (lfo[s] - (1 << 23));
                    pmod = (int32_t)(((int64_t)pmd * (int64_t)senslfo) >> 39);
                    const int32_t pmodWheel = (int32_t)(((int64_t)ctrls->pitch_mod * (int64_t)senslfo) >> 14);
                    pmod = std::abs(pmod) >= std::abs(pmodWheel) ? pmod : pmodWheel;
                }
                f[s] = rate_->freqlut.lookup(pitch + peg_[i].getsample() + pmod);
            }
        } else {
            std::fill(f, f + N, rate_->freqlut.lookup(pitch + peg_[i].level()));
        }
        params_[i].freq = f[0];
        phase0[i] = (uint32_t)params_[i].phase;
        uint32_t pt = 0;
        int32_t level = 0;
        for (int s = 0; s < N; s++) {
            if (ampSens_[i] != 0 && ampMod && (s == 0 || amd[s] != amd[s - 1])) {
                const uint32_t sensamp = (uint32_t)(((uint64_t)amd[s] * (uint64_t)ampSens_[i]) >> 24);
                // (less its value at zero depth: the DX7 code lowers every level by 1.2 % even
                // without modulation, which would slow every Extended envelope by as much)
                pt = (uint32_t)(std::exp(((float)sensamp) / 262144 * 0.07 + 12.2) - std::exp(12.2f));
            }
            level = env_[i].getsample();
            if (pt != 0)
                level -= (int32_t)(((uint64_t)level * ((uint64_t)pt << 4)) >> 28);
            gains_[i][s] = Exp2::lookup(level - (14 * (1 << 24)));
        }
        params_[i].level_in = level;
    }
    ExtKernels k;
    k.gainOf = gains_;
    k.freqOf = freqs_;
    k.base = params_;
    k.fbGain = fbGain_;
    k.waves = waves_;
    renderGraph(k, extCompiled(algorithm_), buf, params_, fbBuf_, fbShift_, bufs_, sum_.get());
    // every operator moved by its per-sample increments (renderGraph assumed `freq` throughout)
    for (int i = 0; i < ext::Ops; i++) {
        uint32_t ph = phase0[i];
        for (int s = 0; s < N; s++)
            ph += (uint32_t)freqs_[i][s];
        params_[i].phase = (int32_t)ph;
    }
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
