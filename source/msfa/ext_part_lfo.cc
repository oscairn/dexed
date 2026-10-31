/*
 * The Extended profile's part LFO (docs/oscairn-plan.md, milestone M6).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "ext_part_lfo.h"

#include <algorithm>
#include <cmath>

#include "ext_tables.h"
#include "ext_voice.h"

void ExtPartLfo::configure(const ExtendedVoice &v, double tempo) {
    using namespace ext;
    wave_ = std::clamp(v.common(PartLfoWave), 0, (int)User);
    const double hz = v.common(PartLfoSync) ? std::max(tempo, 1.0) / 60.0 / extPartLfoTempoBeats(v.common(PartLfoTempoSpeed))
                                            : extPartLfoHz(v.common(PartLfoSpeed));
    inc_ = hz / sampleRate_;
    static const double phases[6] = { 0, 0.25, 1 / 3.0, 0.5, 2 / 3.0, 0.75 };
    startPhase_ = phases[std::clamp(v.common(PartLfoPhase), 0, 5)];
    keyReset_ = std::clamp(v.common(PartLfoKeyReset), 0, 2);
    const double ms = sampleRate_ / 1000.0;
    stage_[0] = extPartLfoDelayMs(v.common(PartLfoDelay)) * ms;
    stage_[1] = extPartLfoFadeInMs(v.common(PartLfoFadeIn)) * ms;
    holdForever_ = v.common(PartLfoHold) >= 127;
    stage_[2] = holdForever_ ? 0 : extPartLfoHoldMs(v.common(PartLfoHold)) * ms;
    stage_[3] = extPartLfoFadeOutMs(v.common(PartLfoFadeOut)) * ms;
    static const int cycles[7] = { 2, 3, 4, 6, 8, 12, 16 };
    steps_ = cycles[std::clamp(v.common(PartLfoUserCycle), 0, 6)];
    slope_ = std::clamp(v.common(PartLfoUserSlope), 0, 3);
    for (int i = 0; i < 16; i++) {
        const int s = std::clamp(v.common((Common)(PartLfoStep1 + i)), -64, 63);
        stepValue_[i] = (s == 63 ? 64 : s) / 64.0f; // +63 plays as full scale (measured)
    }
    lfo2Speed_ = 0;
    for (int d = 0; d < 3; d++)
        if (v.common((Common)(PartLfoDest1 + 2 * d)) == PartLfoLfo2Speed)
            lfo2Speed_ += std::clamp(v.common((Common)(PartLfoDepth1 + 2 * d)), 0, 127) * 100 / 128.0;
    active_ = false;
    for (int d = 0; d < 3; d++)
        active_ = active_ || (v.common((Common)(PartLfoDest1 + 2 * d)) != 0 && v.common((Common)(PartLfoDepth1 + 2 * d)) != 0);
}

void ExtPartLfo::keydown(bool firstKey) {
    if (keyReset_ == 1 || (keyReset_ == 2 && firstKey)) {
        phase_ = startPhase_;
        held_ = 2; // (draw a new value at once)
    }
    // shortcut: the envelope restarts with the phase, and on every first key-on with key-on
    // reset off (the reference's envelope with reset off was not measured)
    if (keyReset_ == 1 || firstKey) {
        envStage_ = 0;
        envPos_ = 0;
    }
}

double ExtPartLfo::wave(double p) {
    switch (wave_) {
    case Triangle: // from 0 up to +1, down to -1, back to 0 (measured)
        return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
    case TrianglePlus: // 0 to +1 and back
        return p < 0.5 ? 2 * p : 2 - 2 * p;
    case SawUp: // 0 to +1
        return p;
    case SawDown: // 0 to -1
        return -p;
    case Square14: return p < 0.25 ? 1 : -1;
    case Square13: return p < 1 / 3.0 ? 1 : -1;
    case Square: return p < 0.5 ? 1 : -1;
    case Square23: return p < 2 / 3.0 ? 1 : -1;
    case Square34: return p < 0.75 ? 1 : -1;
    case Trapezoid: { // the triangle doubled and clipped
        const double t = p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
        return std::clamp(2 * t, -1.0, 1.0);
    }
    case SampleHold1:
    case SampleHold2:
        return held_;
    default: { // user: steps, with slopes up (towards a higher next step), down or both
        const double x = p * steps_;
        const int i = std::min(steps_ - 1, (int)x);
        const double a = stepValue_[i], b = stepValue_[(i + 1) % steps_], f = x - i;
        const bool ramp = slope_ == 3 || (slope_ == 1 && b > a) || (slope_ == 2 && b < a);
        return ramp ? a + (b - a) * f : a;
    }
    }
}

void ExtPartLfo::render(float *out, int n) {
    for (int s = 0; s < n; s++) {
        // envelope
        // (stages of no length pass at once)
        while (envStage_ < 4 && envPos_ >= stage_[envStage_]) {
            envPos_ = 0;
            envStage_++;
            if (envStage_ == 2 && holdForever_) {
                envStage_ = 4;
                envDone_ = 1;
            } else if (envStage_ == 4) {
                envDone_ = 0;
            }
        }
        double env = envDone_;
        if (envStage_ < 4) {
            const double f = envPos_ / stage_[envStage_];
            env = envStage_ == 0 ? 0 : envStage_ == 1 ? f : envStage_ == 2 ? 1 : 1 - f;
            envPos_++;
        }
        if ((wave_ == SampleHold1 || wave_ == SampleHold2) && held_ > 1) {
            // shortcut: both sample and hold waves hold a uniform value per cycle (S/H2's
            // difference from S/H1 was not resolved)
            random_ ^= random_ << 13;
            random_ ^= random_ >> 17;
            random_ ^= random_ << 5;
            held_ = (random_ >> 8) / 8388608.0 - 1.0;
        }
        out[s] = (float)(wave(phase_) * env);
        phase_ += inc_;
        if (phase_ >= 1) {
            phase_ -= std::floor(phase_);
            if (wave_ == SampleHold1 || wave_ == SampleHold2)
                held_ = 2;
        }
    }
}
