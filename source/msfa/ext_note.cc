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
constexpr double kExtendedOutputDb = -6.57; // Extended carriers' overall level (see setup)
const int32_t kNoPitch[N] = {};

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
    // operator op's wave at phase ph, modulated by m (its modulators and feedback)
    int32_t wave(int op, int32_t ph, int32_t m) const {
        const ExtNote::Wave &w = waves[op];
        const uint32_t p = (uint32_t)ph;
        switch (w.kind) {
        case ExtNote::Wave::Table:
            return extwave::lookup(w.table, (int32_t)(p + (uint32_t)m));
        case ExtNote::Wave::Formant:
            return (int32_t)(((int64_t)extwave::lookup(w.table, (int32_t)(p + (uint32_t)m)) *
                              Sin::lookup((int32_t)(p * (uint32_t)w.centre + (uint32_t)m))) >> 24);
        case ExtNote::Wave::Silent:
            return 0;
        default:
            return Sin::lookup((int32_t)(p + (uint32_t)m));
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
            const int32_t y = (int32_t)(((int64_t)wave(cur, ph, in[i]) * g[i]) >> 24);
            o[i] = add ? o[i] + y : y;
        }
    }
    void compute_pure(int32_t *o, int32_t ph, int32_t, int32_t g1, int32_t g2, bool add) {
        int32_t g[N];
        ramp(g1, g2, g);
        const int32_t *f = freqOf[cur];
        for (int i = 0; i < N; i++, ph += f[i - 1]) {
            const int32_t y = (int32_t)(((int64_t)wave(cur, ph, 0) * g[i]) >> 24);
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
            y = (int32_t)(((int64_t)wave(cur, ph, (in ? in[i] : 0) + scaled) * g[i]) >> 24);
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
                const int32_t out = (int32_t)(((int64_t)wave(op, ph[k], in) * gain) >> 24);
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
                       double sampleRate, double attackScale, bool modulator) {
    constexpr double q32 = 4294967296.0;
    full_ = (int64_t)outlevel << 8;
    for (int i = 0; i < 4; i++) {
        levels_[i] = std::clamp(levels[i], 0, 99);
        const double db = modulator ? extModEgLevelDb(levels_[i]) : extEgLevelDb(levels_[i]);
        targets_[i] = std::max<int64_t>(0, ((int64_t)outlevel << 8) + (int64_t)std::llround(db * kLog2PerDb * q32));
        // segment i (attack, decay 1, decay 2, release): dB per sample
        const double msPerDb = (i == 0 ? std::clamp(extAttackMsPerDb(std::clamp(times[i], 0, 99)) * attackScale,
                                                    extAttackMsPerDb(0), extAttackMsPerDb(99))
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
                  double sampleRate, double attackScale, bool modulator) {
    configure(levels, times, holdTime, outlevel, timeScale, sampleRate, attackScale, modulator);
    down_ = true;
    level_ = targets_[3]; // the hold level
    count_ = holdSamples_;
    ix_ = count_ > 0 ? 0 : 1;
}

void ExtEnv::update(const int levels[4], const int times[4], int holdTime, int32_t outlevel, double timeScale,
                    double sampleRate, double attackScale, bool modulator) {
    configure(levels, times, holdTime, outlevel, timeScale, sampleRate, attackScale, modulator);
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

// ---- common pitch EG ----

void ExtPartPitchEnv::init(const double cents[5], const double centsPerMs[4], double sampleRate) {
    auto q = [](double c) { return (int64_t)std::llround(c / 1200.0 * kQ24) << 16; };
    level_ = q(cents[0]);
    for (int i = 0; i < 4; i++) {
        target_[i] = q(cents[i + 1]);
        // (rate 0: instant)
        inc_[i] = centsPerMs[i] <= 0 ? INT64_MAX / 4
                                     : std::max<int64_t>(1, std::llround(centsPerMs[i] / 1200.0 * kQ24 * 65536.0 * 1000.0 / sampleRate));
    }
    ix_ = 0;
}

int32_t ExtPartPitchEnv::getsample() {
    while (ix_ < 4) {
        const int64_t t = target_[ix_];
        if (level_ < t)
            level_ = std::min(t, level_ + inc_[ix_]);
        else if (level_ > t)
            level_ = std::max(t, level_ - inc_[ix_]);
        if (level_ != t)
            break;
        // decay 2's level holds while the key is down; release ends the EG
        ix_ = ix_ >= 2 ? 4 : ix_ + 1;
        if (ix_ == 4 || target_[ix_] != level_)
            break;
    }
    return level();
}

void ExtPartPitchEnv::keyup() { ix_ = 3; }

// ---- filter EG ----

void ExtFilterEnv::configure(const int levels[5], const int times[5], double timeScale, double sampleRate) {
    for (int i = 0; i < 5; i++) {
        target_[i] = extFegLevelCents(levels[i]);
        samples_[i] = (i == 0 ? extFegHoldMs(times[i]) : extFegSegmentMs(times[i])) * timeScale * sampleRate / 1000.0;
    }
}

void ExtFilterEnv::enter(int ix) {
    ix_ = ix;
    if (ix_ >= 5)
        return;
    left_ = samples_[ix_];
    step_ = ix_ == 0 ? 0 : (target_[ix_] - level_) / std::max(left_, 1.0);
}

void ExtFilterEnv::init(const int levels[5], const int times[5], double timeScale, double sampleRate) {
    configure(levels, times, timeScale, sampleRate);
    down_ = true;
    level_ = target_[0];
    enter(0);
}

void ExtFilterEnv::update(const int levels[5], const int times[5], double timeScale, double sampleRate) {
    configure(levels, times, timeScale, sampleRate);
}

double ExtFilterEnv::getblock(int n) {
    double todo = n;
    while (todo > 0 && ix_ < 5) {
        if (ix_ == 3 && left_ <= 0) {
            level_ = target_[3]; // decay 2 holds its level while the key is down
            break;
        }
        const double k = std::min(todo, left_);
        level_ += step_ * k;
        left_ -= k;
        todo -= k;
        if (left_ <= 0) {
            if (ix_ > 0)
                level_ = target_[ix_];
            if (ix_ < 3)
                enter(ix_ + 1);
            else if (ix_ == 4)
                ix_ = 5;
        }
    }
    return level_;
}

void ExtFilterEnv::keydown(bool down) {
    if (down_ == down)
        return;
    down_ = down;
    if (!down)
        enter(4);
}

// ---- voice ----

void ExtNote::chooseWave(int i, int w) {
    Wave &wv = waves_[i];
    if (w == 0) {
        wv = Wave {};
    } else if (w <= 4) {
        wv.kind = Wave::Table;
        wv.table = extwave::table(w, spread_[i], below_[i]);
    } else {
        wv.centre = formant_[i] + 1;
        wv.table = extwave::window(w, spread_[i], below_[i] - wv.centre);
        wv.kind = wv.table ? Wave::Formant : Wave::Silent;
    }
}

void ExtNote::setFeedback(int feedback) {
    fbShift_ = feedback != 0 ? 8 - feedback : 16; // (16: off, for renderGraph)
    // the measured feedback amount, relative to the DX7's shift for the same value
    fbGain_ = feedback != 0 ? std::llround(extFeedbackScale(feedback) * (double)(1 << 24) / (1 << fbShift_)) : 0;
    // the amount was fitted to a carrier's output before carriers got the reference's output
    // level (algorithm gain and overall level); a carrier's feedback takes its signal before
    // that level, so the fitted modulation stays
    const CompiledAlgorithm &ca = extCompiled(algorithm_);
    if (fbGain_ != 0 && ca.feedbackFrom >= 0 && ((ca.outputs >> ca.feedbackFrom) & 1))
        fbGain_ = std::llround((double)fbGain_ * std::pow(10.0, -(extAlgorithmGainDb(algorithm_ + 1) + kExtendedOutputDb) / 20));
}

void ExtNote::setup(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning,
                    bool start) {
    rate_ = rate;
    const double sampleRate = rate->sampleRate;
    algorithm_ = std::clamp(v.common(ext::Algorithm), 1, 88) - 1;
    feedback_ = std::clamp(v.common(ext::Feedback), 0, 7);
    setFeedback(feedback_);
    // the part LFO's feedback destinations: feedback steps at the LFO's top (depth / 16, the
    // move rounded towards 0: measured at depths 8, 32 and 127 from feedback 0, 4 and 7)
    partLfoFeedback_ = 0;
    for (int d = 0; d < 3; d++)
        if (v.common((ext::Common)(ext::PartLfoDest1 + 2 * d)) == ext::PartLfoFeedback)
            partLfoFeedback_ += std::clamp(v.common((ext::Common)(ext::PartLfoDepth1 + 2 * d)), 0, 127) / 16.0;
    detuneNote_ = midinote;
    pitchDepth_ = extLfoPitchDepthSteps(v.common(ext::LfoPitchDepth));
    ampLfo_ = (float)(extLfoAmpDb(v.common(ext::LfoAmpDepth)) * kLog2PerDb * kQ24);
    const int32_t notePitch = tuning.midinote_to_logfreq(midinote);
    notePitch_ = notePitch;
    if (start)
        glide_ = glideStep_ = 0;
    // pitch key follow and random pitch: ratio-mode operators only (fixed ones keep their
    // frequency in the reference)
    const double ratioCents =
        extPitchKeyFollowCents(v.common(ext::PitchKeyFollow), midinote, v.common(ext::PitchKeyCentre)) +
        extRandomPitchCents(v.common(ext::RandomPitch), random_);
    const double partCents = extPartDetuneCents(v.common(ext::PartDetune)); // (shortcut: unmeasured on fixed operators)
    // the part's AEG offsets (every operator; a positive attack offset reaches carriers only) and
    // the macros (modulators only), added to the operators' values
    const double aegAttack = extAegAttackScale(v.common(ext::AegAttackOffset));
    const int aegDecay = extAegOffsetSteps(v.common(ext::AegDecayOffset)),
              aegSustain = extAegSustainSteps(v.common(ext::AegSustainOffset)),
              aegRelease = extAegOffsetSteps(v.common(ext::AegReleaseOffset));
    auto macro = [&](ext::Common k) { return std::clamp((int)v.common(k), -99, 99); };
    const int feedbackFrom = extCompiled(algorithm_).feedbackFrom; // (FmOpParams index)
    reset_ = 0;
    for (int n = 1; n <= ext::Ops; n++) {
        const int i = ext::Ops - n; // FmOpParams index (compileAlgorithm's order)
        auto o = [&](ext::Op k) { return v.op(n, k); };
        if (o(ext::KeyOnReset))
            reset_ |= (uint8_t)(1u << i);
        const bool modulator = !((extCompiled(algorithm_).outputs >> i) & 1);
        double textureDb = 0; // the texture macro's level change
        // frequency
        fixed_[i] = o(ext::Mode) != 0;
        double cents = extPitchVelocityCents(v.common(ext::PitchVelocityDepth), o(ext::PitchVelocity), velocity) + partCents;
        int32_t pitch;
        if (fixed_[i]) {
            cents += extKeyFollowCents(o(ext::KeyFollow), midinote);
            pitch = (int32_t)std::lround(std::log2(extFixedHz(o(ext::Coarse), o(ext::Fine))) * kQ24);
        } else {
            cents += extDetuneCents(o(ext::Detune), midinote) + ratioCents;
            detune_[i] = std::clamp(o(ext::Detune), -15, 15);
            // the ratio macro: ratio-mode modulators (shortcut: unmeasured on fixed-frequency ones)
            double ratio = extRatio(o(ext::Coarse), o(ext::Fine));
            if (modulator)
                ratio = extShiftedRatio(ratio, macro(ext::MacroRatio));
            pitch = notePitch + (int32_t)std::lround(std::log2(ratio) * kQ24);
        }
        basepitch_[i] = pitch + (int32_t)std::lround(cents / 1200.0 * kQ24);
        // the wave, band-limited for this note's frequency (shortcut: chosen at note-on and
        // on edits; pitch EG, LFO and bend moves are not followed)
        {
            const double hz = rate_->freqlut.lookup(basepitch_[i]) * sampleRate / kQ24;
            below_[i] = (int)std::floor(sampleRate / 2 / std::max(hz, 1.0)); // harmonics below Nyquist
            wave_[i] = std::clamp(o(ext::Wave), 0, 6);
            spread_[i] = std::clamp(o(ext::WaveSpread), 0, 7);
            formant_[i] = std::clamp(o(ext::WaveFormant), 0, 99);
            textureDb = 0;
            extTextureWave(macro(ext::MacroTexture), modulator, wave_[i], spread_[i], formant_[i], textureDb);
            chooseWave(i, wave_[i]);
            waveNow_[i] = wave_[i];
        }
        // level: operator level, scaling and velocity, in dB, then MSFA's log format
        // modulators have their own level curve (the modulation index the reference plays)
        int level = o(ext::Level);
        // the depth macro: the modulator's level plus the macro's value, divided by 1 + its feedback
        // when it is the operator that feeds back, rounded towards 0 (measured at feedback 0, 1, 2,
        // 4 and 7, and on a stack of three modulators)
        if (modulator)
            level += macro(ext::MacroDepth) / (1 + (i == feedbackFrom ? feedback_ : 0));
        double db = (modulator ? extModLevelDb(std::clamp(level, 0, 99)) : extOpLevelDb(std::clamp(level, 0, 99))) +
                    textureDb;
        db += extLevelScalingDb(midinote, o(ext::BreakPoint), o(ext::LowDepth), o(ext::HighDepth), o(ext::LowCurve),
                                o(ext::HighCurve));
        const double velocityDb = extLevelVelocityDb(o(ext::LevelVelocity), velocity_ > 0 ? velocity_ : velocity); // (may raise the level)
        // carriers: the reference's output level for this algorithm (up to 13.6 dB above
        // algorithm 1), and its overall level: its probes play a full-level sine 6.57 dB below
        // Octet's Classic-scale carrier, which keeps the loudest algorithms from clipping
        const double algorithmGain = modulator ? 0.0 : extAlgorithmGainDb(algorithm_ + 1) + kExtendedOutputDb;
        const int32_t outlevel = kFullScale + dbToLog(std::min(0.0, db) + velocityDb + algorithmGain);
        int levels[4] = { o(ext::AttackLevel), o(ext::Decay1Level), o(ext::Decay2Level), o(ext::ReleaseLevel) };
        int times[4] = { o(ext::AttackTime), o(ext::Decay1Time), o(ext::Decay2Time), o(ext::ReleaseTime) };
        times[1] += aegDecay;
        times[2] += aegDecay;
        levels[2] += aegSustain;
        times[3] += aegRelease;
        if (modulator) {
            // the envelope macros: one time or level value per step (measured at +-25, +-50, +-99)
            times[0] += macro(ext::MacroAttack);
            times[1] += macro(ext::MacroDecay);
            times[2] += macro(ext::MacroDecay);
            levels[2] += macro(ext::MacroSustain);
            times[3] += macro(ext::MacroRelease);
        }
        for (int k = 0; k < 4; k++) {
            levels[k] = std::clamp(levels[k], 0, 99);
            times[k] = std::clamp(times[k], 0, 99);
        }
        const double timeScale = extTimeKeyFollowScale(o(ext::TimeKeyFollow), midinote);
        const double attackScale = modulator ? std::min(aegAttack, 1.0) : aegAttack;
        if (start)
            env_[i].init(levels, times, o(ext::HoldTime), outlevel, timeScale, sampleRate, attackScale, modulator);
        else
            env_[i].update(levels, times, o(ext::HoldTime), outlevel, timeScale, sampleRate, attackScale, modulator);
        if (start)
            peg_[i].init(o(ext::PegInitial), o(ext::PegAttack), o(ext::PegAttackTime), o(ext::PegDecayTime), sampleRate);
        pitchSens_[i] = extPitchDepthOffset(std::clamp(o(ext::PitchDepthOffset), 0, 7));
        // the part LFO's operator frequency destinations that reach this operator: the depth
        // times the operator's ratio / 128 rounded down, ratio 127 as 128 (measured); ratio mode
        // only, as the 2nd LFO (shortcut: unmeasured on fixed-frequency operators)
        partLfoPitch_[i] = 0;
        if (!fixed_[i])
            for (int d = 0; d < 3; d++)
                if (v.common((ext::Common)(ext::PartLfoDest1 + 2 * d)) == ext::PartLfoOpFrequency && o((ext::Op)(ext::PartLfoBox1 + d))) {
                    const int ratio = std::clamp(o((ext::Op)(ext::PartLfoRatio1 + d)), 0, 127);
                    const int depth = std::clamp(v.common((ext::Common)(ext::PartLfoDepth1 + 2 * d)), 0, 127) *
                                      (ratio == 127 ? 128 : ratio) / 128;
                    partLfoPitch_[i] += (float)(extPartLfoPitchCents(depth) / 1200.0 * kQ24);
                }
        // operator detune destinations: detune steps at the LFO's top, depth / 4 after the ratio
        // rule, the move rounded towards 0 and the detune held within +-15 (measured at depths 8,
        // 32, 64 and 127 at notes 36, 60 and 96)
        partLfoDetune_[i] = 0;
        if (!fixed_[i])
            for (int d = 0; d < 3; d++)
                if (v.common((ext::Common)(ext::PartLfoDest1 + 2 * d)) == ext::PartLfoOpDetune && o((ext::Op)(ext::PartLfoBox1 + d))) {
                    const int ratio = std::clamp(o((ext::Op)(ext::PartLfoRatio1 + d)), 0, 127);
                    partLfoDetune_[i] += std::clamp(v.common((ext::Common)(ext::PartLfoDepth1 + 2 * d)), 0, 127) *
                                         (ratio == 127 ? 128 : ratio) / 128 / 4.0;
                }
        // operator wave destinations: depth / 16 wave steps at the LFO's top (after the ratio
        // rule), rounded towards 0, held within 0-6 (measured with a square wave at depths 8, 32
        // and 127 from all harmonics 1 and formant 1)
        partLfoWave_[i] = 0;
        for (int d = 0; d < 3; d++)
            if (v.common((ext::Common)(ext::PartLfoDest1 + 2 * d)) == ext::PartLfoOpWave && o((ext::Op)(ext::PartLfoBox1 + d))) {
                const int ratio = std::clamp(o((ext::Op)(ext::PartLfoRatio1 + d)), 0, 127);
                partLfoWave_[i] += std::clamp(v.common((ext::Common)(ext::PartLfoDepth1 + 2 * d)), 0, 127) *
                                   (ratio == 127 ? 128 : ratio) / 128 / 16.0;
            }
        // operator level destinations (the same depth and ratio rule, unmeasured for level)
        partLfoLevel_[i] = 0;
        for (int d = 0; d < 3; d++)
            if (v.common((ext::Common)(ext::PartLfoDest1 + 2 * d)) == ext::PartLfoOpLevel && o((ext::Op)(ext::PartLfoBox1 + d))) {
                const int ratio = std::clamp(o((ext::Op)(ext::PartLfoRatio1 + d)), 0, 127);
                const int depth = std::clamp(v.common((ext::Common)(ext::PartLfoDepth1 + 2 * d)), 0, 127) *
                                  (ratio == 127 ? 128 : ratio) / 128;
                partLfoLevel_[i] += (float)(extPartLfoLevelDb(depth) * kLog2PerDb * kQ24);
            }
        ampSens_[i] = extAmpDepthOffset(std::clamp(o(ext::AmpDepthOffset), 0, 7));
        ampShare_[i] = (float)extAmpDepthShare(o(ext::AmpDepthOffset));
    }
    if (start) {
        // the common pitch EG: levels by depth; velocity scales levels and rates alike (the
        // reference's segment times do not change with velocity); time key follow by note
        using namespace ext;
        const int depth = std::clamp(v.common(PitchEgDepth), 0, 3);
        const double vel = extPartPegVelocityScale(v.common(PitchEgVelocity), velocity);
        const double keyScale = extPartPegTimeKeyFollowScale(v.common(PitchEgTimeKeyFollow), midinote);
        double cents[5], rates[4];
        for (int k = 0; k < 5; k++)
            cents[k] = extPartPegCents(v.common((Common)(PitchEgInitial + k)), depth) * vel;
        for (int k = 0; k < 4; k++)
            rates[k] = extPartPegCentsPerMs(v.common((Common)(PitchEgAttackTime + k))) * vel / keyScale;
        ppeg_.init(cents, rates, sampleRate);
    }
    setupFilter(v, midinote, velocity, start);
}

void ExtNote::init(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning,
                   const ExtStart &st) {
    initialised_ = true;
    random_ = st.randomPitch;
    velocity_ = st.velocity;
    {
        // pan: alternate (this side on the sound's first note, the other on the next, ...),
        // scaling and random positions added
        using namespace ext;
        const int alt = std::clamp(v.common(AlternatePan), -64, 63);
        const int pos = ((st.count & 1) ? -alt : alt) + extScalingPanPosition(v.common(ScalingPan), midinote) +
                        extRandomPanPosition(v.common(RandomPan), st.randomPan);
        panPos_ = std::clamp(pos, -64, 63);
        extPanGains(panPos_, gainL_, gainR_);
        // the part LFO's pan destinations: positions at the LFO's top
        partLfoPan_ = 0;
        for (int d = 0; d < 3; d++)
            if (v.common((Common)(PartLfoDest1 + 2 * d)) == PartLfoPan)
                partLfoPan_ += extPartLfoPanPositions(v.common((Common)(PartLfoDepth1 + 2 * d)));
    }
    {
        using namespace ext;
        const double tempo = st.tempo;
        const double seconds = v.common(KeyOnDelaySync) ? extKeyOnDelayBeats(v.common(KeyOnDelayNote)) * 60.0 / std::max(tempo, 1.0)
                                                        : extKeyOnDelayMs(v.common(KeyOnDelay)) / 1000.0;
        delay_ = std::llround(seconds * rate->sampleRate) + st.wait;
        shift_ = 0;
    }
    lfo_.init(rate->sampleRate);
    lfo_.reset(v);
    lfo_.start(st.lfoPhase);
    setup(v, midinote, velocity, rate, tuning, true);
}

void ExtNote::update(const ExtendedVoice &v, int midinote, int velocity, const SynthRate *rate, TuningState &tuning) {
    lfo_.reset(v); // (keeps its phase and delay)
    setup(v, midinote, velocity, rate, tuning, false);
}

void ExtNote::setupFilter(const ExtendedVoice &v, int midinote, int velocity, bool start) {
    using namespace ext;
    FilterState &f = filt_;
    f.type = std::clamp(v.common(FilterType), 0, (int)extfilter::TypeCount - 1);
    // scaling: an offset by note, straight between the break points and constant outside
    // them, 37.5 cents per step (measured below, between and above the break points)
    double offset = 0;
    {
        int bp[4], off[4];
        for (int i = 0; i < 4; i++) {
            bp[i] = std::clamp(v.common((Common)(FilterBreak1 + i)), 0, 127);
            off[i] = std::clamp(v.common((Common)(FilterOffset1 + i)), -128, 127);
        }
        if (midinote <= bp[0])
            offset = off[0];
        else if (midinote >= bp[3])
            offset = off[3];
        else
            for (int i = 1; i < 4; i++)
                if (midinote <= bp[i]) {
                    offset = bp[i] == bp[i - 1] ? off[i] : off[i - 1] + (off[i] - off[i - 1]) * (double)(midinote - bp[i - 1]) / (bp[i] - bp[i - 1]);
                    break;
                }
    }
    // the part's offsets: 8 cutoff values, 2 resonance values and 2 FEG depth values per step
    // (each measured at 6 offsets through HPF12 against the equivalent value)
    const int resonance = std::clamp(v.common(FilterResonance) + 2 * std::clamp((int)v.common(ResonanceOffset), -64, 63), 0, 127);
    const int fegDepth = std::clamp(v.common(FegDepth) + 2 * std::clamp((int)v.common(FegDepthOffset), -64, 63), -64, 63);
    f.value = std::clamp(v.common(FilterCutoff) + 8 * std::clamp((int)v.common(CutoffOffset), -64, 63), 0, 1023);
    f.ratio = extFilterScalingRatio(offset) * extFilterKeyFollowRatio(v.common(FilterKeyFollow), midinote) *
              extFilterVelocityRatio(v.common(FilterCutoffVelocity), velocity);
    f.resonance = resonance;
    f.qVelocity = extFilterResonanceVelocityQ(v.common(FilterResonanceVelocity), velocity);
    f.q = extFilterQ(f.resonance) * f.qVelocity;
    // the part LFO's resonance destination: one resonance step per depth step at its top, the
    // move rounded towards 0 (measured at depths 8 and 32 with a square wave through HPF12;
    // shortcut: BPFw's width does not move, unmeasured)
    f.partLfoRes = 0;
    for (int d = 0; d < 3; d++)
        if (v.common((Common)(PartLfoDest1 + 2 * d)) == PartLfoResonance)
            f.partLfoRes += std::clamp(v.common((Common)(PartLfoDepth1 + 2 * d)), 0, 127);
    f.ratio2 = f.type == extfilter::Lpf18S ? extFilterLpf18sRatio(f.value)
                                           : std::exp2(std::clamp(v.common(FilterDistance), -128, 127) / 16.0);
    extFilterWidth(resonance, f.widthHigh, f.widthLow); // (shortcut: the offset on BPFw's width is unmeasured)
    f.hpf = extFilterCutoffHz(std::clamp(v.common(HpfCutoff), 0, 1023)) *
            extFilterKeyFollowRatio(v.common(HpfKeyFollow), midinote);
    f.gain = extFilterGain(v.common(FilterGain));
    f.fegOctaves = extFegDepth(fegDepth) *
                   extFegDepthVelocity(v.common(FegDepthVelocity), v.common(FegDepthVelocityCurve), velocity) /
                   1200.0; // (the FEG runs in cents)
    f.lfoOctaves = extFilterLfoOctaves(v.common(LfoFilterDepth));
    // the part LFO moves the cutoff value itself: 8 values per depth step at its top (measured
    // at depths 2-32 with a square wave, within 0.2 values per step)
    f.partLfoValues = 0;
    for (int d = 0; d < 3; d++)
        if (v.common((Common)(PartLfoDest1 + 2 * d)) == PartLfoCutoff)
            f.partLfoValues += 8.0 * std::clamp(v.common((Common)(PartLfoDepth1 + 2 * d)), 0, 127);
    f.active = f.type != extfilter::Thru || f.gain != 1.0;
    const int levels[5] = { v.common(FegHoldLevel), v.common(FegAttackLevel), v.common(FegDecay1Level),
                            v.common(FegDecay2Level), v.common(FegReleaseLevel) };
    const int times[5] = { v.common(FegHoldTime), v.common(FegAttackTime), v.common(FegDecay1Time),
                           v.common(FegDecay2Time), v.common(FegReleaseTime) };
    // shortcut: FEG time velocity sensitivity is not applied (the reference shortened the
    // segments at velocity 127 for both -64 and +63 and left them at velocity 1; unresolved)
    const double timeScale = extFegTimeKeyFollowScale(v.common(FegTimeKeyFollow), midinote, v.common(FegTimeKeyCenter));
    if (start) {
        feg_.init(levels, times, timeScale, rate_->sampleRate);
        filter_.reset();
        filterStarted_ = false;
    } else {
        feg_.update(levels, times, timeScale, rate_->sampleRate);
    }
}

// The voice's block through its filter: the operators' sum (buf, MSFA's output scale) in
// floating point, full scale 1.0.
void ExtNote::filterBlock(int32_t *buf, const int32_t *lfo, int32_t lfo_delay, double partLfo) {
    const FilterState &f = filt_;
    const double partValue = partLfo * f.partLfoValues;
    double oct = feg_.getblock(N) * f.fegOctaves;
    // the LFO at the block's end, bipolar (the reference's triangle starts at the cutoff's
    // highest point, as at the pitch's), faded in by its delay
    if (f.lfoOctaves != 0)
        oct += f.lfoOctaves * ((double)lfo[N - 1] / (1 << 23) - 1.0) * ((double)lfo_delay / (1 << 24));
    const double move = std::exp2(oct) * f.ratio;
    ExtFilter::Setting st;
    st.type = f.type;
    st.fc = extFilterCutoffHz(std::clamp(f.value + partValue, 0.0, 1023.0)) * move;
    st.q = f.partLfoRes != 0 && partLfo != 0
               ? extFilterQ(std::clamp(f.resonance + (int)(partLfo * f.partLfoRes), 0, 127)) * f.qVelocity
               : f.q;
    st.ratio2 = f.ratio2;
    if (f.type == extfilter::BpfWide) {
        st.fc = extFilterCutoffHz(std::clamp(f.value + partValue + f.widthHigh, 0.0, 1023.0)) * move;
        st.fcLow = extFilterCutoffHz(std::clamp(f.value + partValue - f.widthLow, 0.0, 1023.0)) * move;
    }
    st.hpf = f.hpf;
    st.gain = f.gain;
    filter_.set(st, rate_->sampleRate, !filterStarted_);
    filterStarted_ = true;
    constexpr float kScale = 1.0f / (float)(1 << 28), kBack = (float)(1 << 28);
    float x[N];
    for (int i = 0; i < N; i++)
        x[i] = (float)buf[i] * kScale;
    filter_.process(x, N);
    for (int i = 0; i < N; i++)
        buf[i] = (int32_t)std::lrint(std::clamp(x[i], -7.99f, 7.99f) * kBack);
}

void ExtNote::compute(int32_t *buf, int32_t pitchBase, const Controllers *ctrls, const float *part_lfo,
                      int lfoSpeedSteps) {
    if (!initialised_)
        return; // (released within its key-on delay)
    if (delay_ >= N) {
        delay_ -= N; // nothing moves before the note starts
        return;
    }
    if (delay_ > 0) {
        shift_ = (int)delay_;
        delay_ = 0;
        std::fill(carry_, carry_ + N, 0);
    }
    if (partLfoPan_ != 0 && part_lfo != nullptr) {
        // pan moved by the part LFO, once per block (its value mid-block)
        const int pos = (int)std::lround(std::clamp(panPos_ + part_lfo[N / 2] * partLfoPan_, -64.0, 63.0));
        extPanGains(pos, gainL_, gainR_);
    }
    lfo_.setSpeedSteps(lfoSpeedSteps);
    if (shift_ == 0) {
        render(buf, pitchBase, ctrls, part_lfo);
        return;
    }
    // started within a block: every block runs shift_ samples late
    int32_t tmp[N] = {};
    render(tmp, pitchBase, ctrls, part_lfo);
    for (int s = 0; s < shift_; s++)
        buf[s] += carry_[s];
    for (int s = shift_; s < N; s++)
        buf[s] += tmp[s - shift_];
    std::copy(tmp + N - shift_, tmp + N, carry_);
}

void ExtNote::render(int32_t *buf, int32_t pitchBase, const Controllers *ctrls, const float *part_lfo) {
    // the voice's own 2nd LFO, every sample; pitch and amplitude modulation as the DX7 voice
    // computes them, per operator
    int32_t lfo[N];
    for (int s = 0; s < N; s++)
        lfo[s] = lfo_.getsample();
    const int32_t lfo_delay = lfo_.getdelay();
    const int64_t pmd = (int64_t)pitchDepth_ * lfo_delay; // Q32 (depth 99 plays as 256: up to 2^32)
    const bool pitchMod = pmd != 0 || ctrls->pitch_mod != 0;
    // the 2nd LFO lowers each operator's level straight in dB, none at its top (measured); the mod
    // wheel's amplitude modulation as the DX7 voice computes it (unmeasured)
    const float ampLfo = ampLfo_ * (float)lfo_delay / (float)(1 << 24);
    float lfoLoss[N]; // log level lost at offset 7
    uint32_t amd[N];
    bool ampMod = false;
    for (int s = 0; s < N; s++) {
        const int32_t lfoInv = (1 << 24) - lfo[s];
        lfoLoss[s] = ampLfo * (float)lfoInv / (float)(1 << 24);
        amd[s] = (uint32_t)(((int64_t)ctrls->amp_mod * (int64_t)lfoInv) >> 7);
        ampMod |= amd[s] != 0;
    }
    // the part LFO's feedback and operator wave moves, once per block
    if (partLfoFeedback_ != 0 && part_lfo != nullptr)
        setFeedback(std::clamp(feedback_ + (int)(part_lfo[N / 2] * partLfoFeedback_), 0, 7));
    if (part_lfo != nullptr)
        for (int i = 0; i < ext::Ops; i++)
            if (partLfoWave_[i] != 0) {
                const int w = std::clamp(wave_[i] + (int)(part_lfo[N / 2] * partLfoWave_[i]), 0, 6);
                if (w != waveNow_[i]) {
                    chooseWave(i, w);
                    waveNow_[i] = w;
                }
            }
    // the common pitch EG, every sample of the block (fixed-frequency operators do not follow
    // it in the reference)
    int32_t ppeg[N];
    const bool ppegMoving = ppeg_.active() || glide_ != 0;
    for (int s = 0; s < N; s++) {
        ppeg[s] = ppeg_.getsample();
        if (glide_ != 0) { // portamento, straight in semitones
            glide_ = glide_ > 0 ? std::max(0.0, glide_ - glideStep_) : std::min(0.0, glide_ + glideStep_);
            ppeg[s] += (int32_t)std::lrint(glide_);
        }
    }
    uint32_t phase0[ext::Ops];
    for (int i = 0; i < ext::Ops; i++) {
        int32_t pitch = basepitch_[i] + pitchBase;
        if (partLfoDetune_[i] != 0 && part_lfo != nullptr) {
            // the part LFO's detune move, once per block
            const int d = std::clamp(detune_[i] + (int)(part_lfo[N / 2] * partLfoDetune_[i]), -15, 15);
            pitch += (int32_t)std::lround((extDetuneCents(d, detuneNote_) - extDetuneCents(detune_[i], detuneNote_)) / 1200.0 * kQ24);
        }
        int32_t *f = freqs_[i];
        const bool lfoPitch = !fixed_[i] && pitchSens_[i] != 0 && pitchMod;
        const int32_t *pp = fixed_[i] ? kNoPitch : ppeg;
        const bool partPitch = part_lfo != nullptr && partLfoPitch_[i] != 0;
        if (lfoPitch || peg_[i].active() || (ppegMoving && !fixed_[i]) || partPitch) {
            for (int s = 0; s < N; s++) {
                int32_t pmod = partPitch ? (int32_t)std::lrint(part_lfo[s] * partLfoPitch_[i]) : 0;
                if (lfoPitch) {
                    const int64_t senslfo = (int64_t)pitchSens_[i] * (lfo[s] - (1 << 23));
                    // twice the DX7's depth (depth 99 with offset 7 swings +-2400 cents; measured at
                    // 17 depths and 14 offsets)
                    const int32_t lfo2 = (int32_t)(((pmd >> 8) * senslfo) >> 30);
                    const int32_t pmodWheel = (int32_t)((ctrls->pitch_mod * senslfo) >> 14);
                    pmod += std::abs(lfo2) >= std::abs(pmodWheel) ? lfo2 : pmodWheel;
                }
                f[s] = rate_->freqlut.lookup(pitch + peg_[i].getsample() + pmod + pp[s]);
            }
        } else {
            std::fill(f, f + N, rate_->freqlut.lookup(pitch + peg_[i].level() + pp[N - 1]));
        }
        params_[i].freq = f[0];
        phase0[i] = (uint32_t)params_[i].phase;
        uint32_t pt = 0;
        int32_t level = 0;
        const bool partLevel = part_lfo != nullptr && partLfoLevel_[i] != 0;
        for (int s = 0; s < N; s++) {
            if (ampSens_[i] != 0 && ampMod && (s == 0 || amd[s] != amd[s - 1])) {
                const uint32_t sensamp = (uint32_t)(((uint64_t)amd[s] * (uint64_t)ampSens_[i]) >> 24);
                // (less its value at zero depth: the DX7 code lowers every level by 1.2 % even
                // without modulation, which would slow every Extended envelope by as much)
                pt = (uint32_t)(std::exp(((float)sensamp) / 262144 * 0.07 + 12.2) - std::exp(12.2f));
            }
            const int32_t env = env_[i].getsample();
            level = env;
            if (pt != 0)
                level -= (int32_t)(((uint64_t)env * ((uint64_t)pt << 4)) >> 28);
            if (ampLfo != 0 && ampShare_[i] != 0) // (the larger of the LFO's and the wheel's loss)
                level = std::max(0, std::min(level, env - (int32_t)std::lrint(lfoLoss[s] * ampShare_[i])));
            if (partLevel) // the part LFO only lowers the level: none at its top, all at its bottom
                level = std::max(0, level - (int32_t)std::lrint((1.0f - part_lfo[s]) * 0.5f * partLfoLevel_[i]));
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
    if (filt_.active) {
        AlignedBuf<int32_t, N> out;
        std::fill(out.get(), out.get() + N, 0);
        renderGraph(k, extCompiled(algorithm_), out.get(), params_, fbBuf_, fbShift_, bufs_, sum_.get());
        filterBlock(out.get(), lfo, lfo_delay, part_lfo != nullptr ? part_lfo[N - 1] : 0.0);
        for (int s = 0; s < N; s++)
            buf[s] += out.get()[s];
    } else {
        renderGraph(k, extCompiled(algorithm_), buf, params_, fbBuf_, fbShift_, bufs_, sum_.get());
    }
    // every operator moved by its per-sample increments (renderGraph assumed `freq` throughout)
    for (int i = 0; i < ext::Ops; i++) {
        uint32_t ph = phase0[i];
        for (int s = 0; s < N; s++)
            ph += (uint32_t)freqs_[i][s];
        params_[i].phase = (int32_t)ph;
    }
}

void ExtNote::keyup() {
    if (delay_ > 0) {
        initialised_ = false; // released before its key-on delay ended: never sounds
        return;
    }
    for (auto &e : env_)
        e.keydown(false);
    feg_.keydown(false);
    ppeg_.keyup();
}

bool ExtNote::isPlaying() const {
    if (!initialised_)
        return false;
    if (delay_ > 0)
        return true;
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
    lfo_ = src.lfo_; // a legato note's 2nd LFO goes on (shortcut: unmeasured in mono legato)
    // and it takes over the key-on delay: none once the sounding note has started, the rest of it
    // while that note still waits (measured in mono legato with portamento)
    delay_ = src.delay_;
    shift_ = src.shift_;
    std::copy(src.carry_, src.carry_ + N, carry_);
    feg_.transfer(src.feg_);
    ppeg_ = src.ppeg_;
    filter_ = src.filter_;
    filterStarted_ = src.filterStarted_;
    for (int i = 0; i < ext::Ops; i++) {
        env_[i].transfer(src.env_[i]);
        params_[i].gain_out = src.params_[i].gain_out;
        params_[i].phase = src.params_[i].phase;
    }
}

void ExtNote::glideFrom(double from, double semitonesPerSecond) {
    glide_ = from - notePitch_;
    glideStep_ = semitonesPerSecond > 0 ? semitonesPerSecond * kQ24 / 12.0 / rate_->sampleRate : 0;
    if (glideStep_ <= 0 || std::isinf(glideStep_))
        glide_ = 0;
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
