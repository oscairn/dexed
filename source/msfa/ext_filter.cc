/*
 * The Extended profile's per-voice filter (docs/oscairn-plan.md, milestone M6).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "ext_filter.h"

#include <algorithm>
#include <cmath>

#include "ext_tables.h"
#include "synth.h"

using namespace extfilter;

namespace {

enum Out { Lp, Bp, Hp, Br };

// shortcut: the reference overloads at high resonance (measured: LPF24D and BPF12D above
// resonance 80 distort); Octet only keeps its states bounded, at 18 dB above full scale
constexpr float kLimit = 8.0f;

} // namespace

void extFilterCoefficients(double fc, double q, double sampleRate, float &F, float &damping) {
    // the reference stops at a sixth of its 44.1 kHz rate (F = 1) and damps more as the cutoff
    // nears that (extFilterMaxDamping)
    fc = std::clamp(fc, 1.0, std::min(extFilterMaxHz(), sampleRate / 6));
    F = (float)(2 * std::sin(M_PI * fc / sampleRate));
    damping = (float)std::min(1 / std::max(q, 0.01), extFilterMaxDamping(fc));
    // and never where the section turns unstable (the limit above keeps it well inside at
    // 44.1 kHz and above; this guards lower rates)
    damping = std::min(damping, 0.95f * (2 / F - F / 2));
}

void ExtFilter::Svf::target(float f, float qq, int n, bool jump) {
    if (jump) {
        F = f;
        q = qq;
        dF = dq = 0;
    } else {
        dF = (f - F) / (float)n;
        dq = (qq - q) / (float)n;
    }
}

float ExtFilter::Svf::tick(float x, int out) {
    F += dF;
    q += dq;
    lp += F * bp;
    const float hp = x - lp - q * bp;
    bp += F * hp;
    lp = std::clamp(lp, -kLimit, kLimit);
    bp = std::clamp(bp, -kLimit, kLimit);
    switch (out) {
    case Lp:
        return lp;
    case Bp:
        return bp;
    case Hp:
        return hp;
    default:
        return lp + hp;
    }
}

void ExtFilter::OnePole::target(float f, int n, bool jump) {
    if (jump) {
        a = f;
        da = 0;
    } else {
        da = (f - a) / (float)n;
    }
}

float ExtFilter::OnePole::tick(float x) {
    a += da;
    y += a * (x - y);
    return y;
}

void ExtFilter::reset() {
    for (Svf &s : s_)
        s.lp = s.bp = 0;
    h_.lp = h_.bp = 0;
    o_.y = 0;
}

void ExtFilter::set(const Setting &st, double sampleRate, bool jump) {
    const int type = std::clamp(st.type, 0, TypeCount - 1);
    if (type != type_) {
        reset();
        jump = true;
    }
    type_ = type;
    n_ = N;
    float F, d, Fh, dh, F1, d1;
    extFilterCoefficients(st.fc, st.q, sampleRate, F, d);
    extFilterCoefficients(st.hpf, 1.0, sampleRate, Fh, dh);
    extFilterCoefficients(st.fc, 1.0, sampleRate, F1, d1);
    // the second section of the dual types and LPF18s: the first's F times a ratio, beyond
    // the first section's limit (measured up to F = 1.75), with the damping only limited
    // where the filter would turn unstable
    // shortcut: the reference's limit above F = 1 is unmeasured except at LPF18s resonance 64
    const float F2 = std::min(F * (float)st.ratio2, 1.75f);
    const float d2 = F2 > 1 ? std::min((float)(1 / std::max(st.q, 0.01)), 0.9f * (2 / F2 - F2 / 2)) : d;
    switch (type) {
    case Lpf24A: // a Q-1 section, then the resonant one
        s_[0].target(F1, d1, n_, jump);
        s_[1].target(F, d, n_, jump);
        break;
    case Lpf18S: // the one-pole at the cutoff, then the resonant section above it
        s_[0].target(F2, d2, n_, jump);
        break;
    case BpfWide: { // high-pass at the low edge, low-pass at the high edge, both Q 1
        float Fl, dl;
        extFilterCoefficients(st.fcLow, 1.0, sampleRate, Fl, dl);
        s_[0].target(Fl, dl, n_, jump);
        s_[1].target(F1, d1, n_, jump);
        break;
    }
    case DualLpf: case DualHpf: case DualBpf: case DualBef:
        s_[0].target(F, d, n_, jump);
        s_[1].target(F2, d2, n_, jump);
        break;
    case Lpf12Bpf6: // the band-pass section's Q is 1.13 times the low-pass's (fitted)
        s_[0].target(F, d, n_, jump);
        s_[1].target(F, d / 1.13f, n_, jump);
        break;
    default:
        s_[0].target(F, d, n_, jump);
        s_[1].target(F, d, n_, jump);
        break;
    }
    h_.target(Fh, dh, n_, jump);
    // the one-pole's coefficient is the cutoff's F
    o_.target(F, n_, jump);
    const float g = (float)st.gain;
    if (jump) {
        gain_ = g;
        dgain_ = 0;
    } else {
        dgain_ = (g - gain_) / (float)n_;
    }
}

void ExtFilter::process(float *buf, int n) {
    auto run = [&](auto f) {
        for (int i = 0; i < n; i++) {
            gain_ += dgain_;
            buf[i] = f(buf[i]) * gain_;
        }
    };
    Svf &a = s_[0], &b = s_[1];
    switch (type_) {
    case Lpf24D: case Lpf24A:
        run([&](float x) { return b.tick(a.tick(x, Lp), Lp); });
        break;
    case Lpf18:
        run([&](float x) { return o_.tick(a.tick(x, Lp)); });
        break;
    case Lpf18S:
        run([&](float x) { return a.tick(o_.tick(x), Lp); });
        break;
    case Lpf12Hpf12:
        run([&](float x) { return h_.tick(a.tick(x, Lp), Hp); });
        break;
    case Lpf6Hpf12:
        run([&](float x) { return h_.tick(o_.tick(x), Hp); });
        break;
    case Hpf24D:
        run([&](float x) { return b.tick(a.tick(x, Hp), Hp); });
        break;
    case Hpf12:
        run([&](float x) { return a.tick(x, Hp); });
        break;
    case Bpf12D:
        run([&](float x) { return b.tick(a.tick(x, Bp), Bp); });
        break;
    case BpfWide:
        run([&](float x) { return b.tick(a.tick(x, Hp), Lp); });
        break;
    case Bpf6:
        run([&](float x) { return a.tick(x, Bp); });
        break;
    case Bef12: case DualBef:
        run([&](float x) { return b.tick(a.tick(x, Br), Br); });
        break;
    case Bef6:
        run([&](float x) { return a.tick(x, Br); });
        break;
    case DualLpf:
        run([&](float x) { return a.tick(x, Lp) + b.tick(x, Lp); });
        break;
    case DualHpf:
        run([&](float x) { return a.tick(x, Hp) + b.tick(x, Hp); });
        break;
    case DualBpf:
        run([&](float x) { return a.tick(x, Bp) + b.tick(x, Bp); });
        break;
    case Lpf12Bpf6:
        run([&](float x) { return a.tick(x, Lp) + b.tick(x, Bp); });
        break;
    default:
        run([&](float x) { return x; });
        break;
    }
}
