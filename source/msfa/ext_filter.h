/*
 * The Extended profile's per-voice filter (docs/oscairn-plan.md, milestone M6).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Every type is built from the same two sections, as the reference's responses show
 * (docs/oscairn-plan.md section 11, M6): a Chamberlin state-variable filter (low-pass,
 * band-pass, high-pass and notch outputs; cutoff coefficient F = 2 sin(pi fc / fs), damping
 * 1/Q) and a one-pole low-pass with the same coefficient F.
 */
#pragma once

#include <cstdint>

namespace extfilter {

// the 19 types, in the documented order (parameter values 0-18)
enum Type {
    Lpf24D, Lpf24A, Lpf18, Lpf18S, Lpf12Hpf12, Lpf6Hpf12, Hpf24D, Hpf12, Bpf12D, BpfWide, Bpf6,
    Bef12, Bef6, DualLpf, DualHpf, DualBpf, DualBef, Lpf12Bpf6, Thru, TypeCount
};

} // namespace extfilter

class ExtFilter {
public:
    // one block's settings; frequencies in Hz
    struct Setting {
        int type = extfilter::Thru;
        double fc = 1000;   // the cutoff
        double q = 0.5;     // the resonance as a Q
        double ratio2 = 1;  // dual types and LPF18s: the second section's coefficient F, times the first's
        double fcLow = 1000; // BPFw: the low edge (fc is the high edge)
        double hpf = 20;    // LPF12+HPF12 and LPF6+HPF12: the high-pass cutoff
        double gain = 1;    // output gain
    };
    void reset();
    // the settings for the next block; the coefficients move to them linearly over the block
    // (at once when `jump`, or when the type changes)
    void set(const Setting &s, double sampleRate, bool jump);
    void process(float *buf, int n);
    int type() const { return type_; }

private:
    struct Svf {
        float lp = 0, bp = 0;
        float F = 0, q = 2, dF = 0, dq = 0;
        void target(float f, float qq, int n, bool jump);
        // one sample; returns the requested output
        float tick(float x, int out);
    };
    struct OnePole {
        float y = 0, a = 0, da = 0;
        void target(float f, int n, bool jump);
        float tick(float x);
    };
    Svf s_[2], h_;
    OnePole o_;
    int type_ = extfilter::Thru;
    float gain_ = 1, dgain_ = 0;
    int n_ = 64;
};

// The coefficients for a cutoff in Hz and a Q: F = 2 sin(pi fc / fs) and damping 1/Q, both
// limited as the reference limits them (exposed for the tests).
void extFilterCoefficients(double fc, double q, double sampleRate, float &F, float &damping);
