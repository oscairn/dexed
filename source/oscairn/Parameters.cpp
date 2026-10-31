/*
 * Octet parameter table.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "Parameters.h"

#include <algorithm>
#include <cmath>

#include "msfa/ext_tables.h"
#include "msfa/lfo.h"

namespace oscairn_fm {

// Same bytes as Program::initialProgram().
const uint8_t initVoice[156] = {
    99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
    99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
    99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
    99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
    99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
    99, 99, 99, 99, 99, 99, 99, 00, 0, 0, 0, 0, 0, 0, 0, 0, 99, 0, 1, 0, 7,
    99, 99, 99, 99, 50, 50, 50, 50, 0, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24,
    'I', 'N', 'I', 'T', ' ', 'V', 'O', 'I', 'C', 'E',
    63
};

static const char *const offOn[] = { "Off", "On", nullptr };
static const char *const engines[] = { "Modern", "Mark I", "OPL", nullptr };
static const char *const lfoWaves[] = { "Triangle", "Saw down", "Saw up", "Square", "Sine", "Sample and hold", nullptr };
static const char *const curves[] = { "-Linear", "-Exponential", "+Exponential", "+Linear", nullptr };
static const char *const modes[] = { "Ratio", "Fixed", nullptr };
static const char *const profiles[] = { "Classic", "Extended", nullptr };
static const char *const phases[] = { "0 degrees", "90 degrees", "180 degrees", "270 degrees", nullptr };
static const char *const speedRanges[] = { "Normal", "Extended", nullptr };
static const char *const waves[] = { "Sine", "All harmonics 1", "All harmonics 2", "Odd harmonics 1",
                                     "Odd harmonics 2", "Formant 1", "Formant 2", nullptr };

static ExtendedVoice buildExtDefaults() {
    using namespace ext;
    ExtendedVoice e {};
    e.v[ProfileSel] = Classic;
    e.v[Algorithm] = 69;
    e.v[LfoSpeed] = 30;
    e.v[LfoSpeedRange] = 1;
    e.v[LfoExtSpeed] = 60;
    for (int n = 1; n <= Ops; n++) {
        int16_t *o = e.v + op(n, KeyOnReset);
        o[KeyOnReset] = 1;
        o[Coarse] = 1;
        o[AttackLevel] = o[Decay1Level] = o[Decay2Level] = 99;
        o[ReleaseTime] = 40;
        o[Level] = n == 8 ? 85 : 0;
        o[BreakPoint] = 39;
        o[PitchDepthOffset] = 3;
        o[AmpDepthOffset] = 3;
    }
    return e;
}

const ExtendedVoice &extDefaults() {
    static const ExtendedVoice e = buildExtDefaults();
    return e;
}

static std::vector<Param> build() {
    std::vector<Param> p;
    auto byte = [&](uint32_t id, const char *module, std::string name, double min, double max, int target, int offset = 0,
                    Text text = Text::Number, const char *const *choices = nullptr) {
        p.push_back({ id, module, std::move(name), min, max, (double)initVoice[target] - offset, Kind::Byte, target, offset, text, choices });
    };
    auto setting = [&](uint32_t id, const char *module, const char *name, double min, double max, double def, Setting s,
                       Text text = Text::Number, const char *const *choices = nullptr) {
        p.push_back({ id, module, name, min, max, def, Kind::Setting, (int)s, 0, text, choices });
    };

    byte(1, "Common", "Algorithm", 1, 32, 134, -1);
    byte(2, "Common", "Feedback", 0, 7, 135);
    byte(3, "Common", "Oscillator Key Sync", 0, 1, 136, 0, Text::Switch, offOn);
    byte(4, "Common", "Transpose", -24, 24, 144, 24, Text::Signed);
    byte(10, "LFO 1", "Speed", 0, 99, 137);
    byte(11, "LFO 1", "Delay", 0, 99, 138);
    byte(12, "LFO 1", "Pitch Depth", 0, 99, 139);
    byte(13, "LFO 1", "Amp Depth", 0, 99, 140);
    byte(14, "LFO 1", "Key Sync", 0, 1, 141, 0, Text::Switch, offOn);
    byte(15, "LFO 1", "Wave", 0, 5, 142, 0, Text::Choice, lfoWaves);
    byte(16, "Pitch", "LFO Pitch Sensitivity", 0, 7, 143);
    for (int i = 0; i < 4; i++) {
        byte(20 + i, "Pitch/EG", "Rate " + std::to_string(i + 1), 0, 99, 126 + i);
        byte(24 + i, "Pitch/EG", "Level " + std::to_string(i + 1), 0, 99, 130 + i);
    }

    // The Extended profile (docs/oscairn-plan.md, section 5): its own values, ranges as
    // documented for FM-X Parts. Shown or hidden by the editor description, never removed.
    const ExtendedVoice &xd = extDefaults();
    auto extv = [&](uint32_t id, const std::string &module, std::string name, double min, double max, int index,
                    Text text = Text::Number, const char *const *choices = nullptr) {
        p.push_back({ id, module, std::move(name), min, max, (double)xd.v[index], Kind::Ext, index, 0, text, choices });
    };
    extv(5, "Common", "Profile", 0, 1, ext::ProfileSel, Text::Choice, profiles);
    extv(40, "Extended/Common", "Algorithm", 1, 88, ext::Algorithm);
    extv(41, "Extended/Common", "Feedback", 0, 7, ext::Feedback);
    extv(42, "Extended/LFO 2", "Wave", 0, 5, ext::LfoWave, Text::Choice, lfoWaves);
    extv(43, "Extended/LFO 2", "Speed", 0, 99, ext::LfoSpeed);
    extv(44, "Extended/LFO 2", "Phase", 0, 3, ext::LfoPhase, Text::Choice, phases);
    extv(45, "Extended/LFO 2", "Delay", 0, 99, ext::LfoDelay);
    extv(46, "Extended/LFO 2", "Key On Reset", 0, 1, ext::LfoKeyReset, Text::Switch, offOn);
    extv(47, "Extended/LFO 2", "Pitch Depth", 0, 99, ext::LfoPitchDepth);
    extv(48, "Extended/LFO 2", "Amp Depth", 0, 99, ext::LfoAmpDepth);
    extv(49, "Extended/LFO 2", "Speed Range", 0, 1, ext::LfoSpeedRange, Text::Choice, speedRanges);
    extv(50, "Extended/LFO 2", "Extended Speed", 0, 415, ext::LfoExtSpeed);
    extv(51, "Extended/Common", "Pitch Velocity Sensitivity", -64, 63, ext::PitchVelocityDepth, Text::Signed);
    for (int n = 1; n <= ext::Ops; n++) {
        const std::string op = "Operator " + std::to_string(n) + "/Extended";
        const std::string env = op + "/Envelope", freq = op + "/Frequency", scale = op + "/Scaling",
                          mod = op + "/Modulation", peg = op + "/Pitch EG";
        const uint32_t id = 1000 * n;
        auto x = [&](uint32_t k, const std::string &module, std::string name, double min, double max, ext::Op o,
                     Text text = Text::Number, const char *const *choices = nullptr) {
            extv(id + k, module, std::move(name), min, max, ext::op(n, o), text, choices);
        };
        x(30, freq, "Key On Reset", 0, 1, ext::KeyOnReset, Text::Switch, offOn);
        x(31, freq, "Mode", 0, 1, ext::Mode, Text::Choice, modes);
        x(32, freq, "Coarse", 0, 31, ext::Coarse);
        x(33, freq, "Fine", 0, 127, ext::Fine);
        x(34, freq, "Detune", -15, 15, ext::Detune, Text::Signed);
        x(35, freq, "Key Follow", 0, 99, ext::KeyFollow);
        x(36, freq, "Pitch Velocity Sensitivity", -7, 7, ext::PitchVelocity, Text::Signed);
        x(37, peg, "Initial Level", -50, 50, ext::PegInitial, Text::Signed);
        x(38, peg, "Attack Level", -50, 50, ext::PegAttack, Text::Signed);
        x(39, peg, "Attack Time", 0, 99, ext::PegAttackTime);
        x(40, peg, "Decay Time", 0, 99, ext::PegDecayTime);
        static const char *const segments[] = { "Attack", "Decay 1", "Decay 2", "Release" };
        for (int i = 0; i < 4; i++)
            x(41 + i, env, std::string(segments[i]) + (i == 3 ? " (Hold) Level" : " Level"), 0, 99, (ext::Op)(ext::AttackLevel + i));
        for (int i = 0; i < 4; i++)
            x(45 + i, env, std::string(segments[i]) + " Time", 0, 99, (ext::Op)(ext::AttackTime + i));
        x(49, env, "Hold Time", 0, 99, ext::HoldTime);
        x(50, env, "Time Key Follow", 0, 7, ext::TimeKeyFollow);
        x(51, op, "Level", 0, 99, ext::Level);
        x(52, scale, "Break Point", 0, 99, ext::BreakPoint, Text::Note);
        x(53, scale, "Low Depth", 0, 99, ext::LowDepth);
        x(54, scale, "High Depth", 0, 99, ext::HighDepth);
        x(55, scale, "Low Curve", 0, 3, ext::LowCurve, Text::Choice, curves);
        x(56, scale, "High Curve", 0, 3, ext::HighCurve, Text::Choice, curves);
        x(57, mod, "Velocity Sensitivity", -7, 7, ext::LevelVelocity, Text::Signed);
        x(58, mod, "LFO Pitch Depth Offset", 0, 7, ext::PitchDepthOffset);
        x(59, mod, "LFO Amp Depth Offset", 0, 7, ext::AmpDepthOffset);
        const std::string wave = op + "/Wave";
        x(60, wave, "Wave", 0, 6, ext::Wave, Text::Choice, waves);
        x(61, wave, "Spread", 0, 7, ext::WaveSpread);
        x(62, wave, "Formant", 0, 99, ext::WaveFormant);
    }

    setting(100, "Voice", "Engine", 0, 2, 1, Setting::Engine, Text::Choice, engines);
    setting(101, "Voice", "Mono", 0, 1, 0, Setting::Mono, Text::Switch, offOn);
    setting(102, "Voice", "Portamento Time", 0, 99, 0, Setting::PortamentoTime);
    setting(103, "Voice", "Glissando", 0, 1, 0, Setting::Glissando, Text::Switch, offOn);
    setting(104, "Pitch", "Bend Range Up", 0, 48, 3, Setting::PitchBendUp);
    setting(105, "Pitch", "Bend Range Down", 0, 48, 3, Setting::PitchBendDown);
    setting(106, "Pitch", "Bend Step", 0, 12, 0, Setting::PitchBendStep);
    static const char *const names[4][4] = {
        { "Mod Wheel Range", "Mod Wheel Pitch", "Mod Wheel Amp", "Mod Wheel EG Bias" },
        { "Foot Range", "Foot Pitch", "Foot Amp", "Foot EG Bias" },
        { "Breath Range", "Breath Pitch", "Breath Amp", "Breath EG Bias" },
        { "Aftertouch Range", "Aftertouch Pitch", "Aftertouch Amp", "Aftertouch EG Bias" } };
    for (int s = 0; s < 4; s++) {
        const int base = (int)Setting::WheelRange + 4 * s;
        setting(110 + 4 * s, "Controllers", names[s][0], 0, 99, 0, (Setting)base);
        for (int f = 1; f < 4; f++)
            setting(110 + 4 * s + f, "Controllers", names[s][f], 0, 1, 0, (Setting)(base + f), Text::Switch, offOn);
    }
    setting(130, "Voice", "Master Tune", -1, 1, 0, Setting::MasterTune, Text::Tune);
    setting(131, "Filter", "Cutoff", 0, 1, 1, Setting::Cutoff, Text::Percent);
    setting(132, "Filter", "Resonance", 0, 1, 0, Setting::Resonance, Text::Percent);
    setting(133, "Amp", "Output", 0, 1, 1, Setting::Output, Text::Percent);

    for (int n = 1; n <= 6; n++) {
        const std::string op = "Operator " + std::to_string(n);
        const std::string env = op + "/Envelope", freq = op + "/Frequency", scale = op + "/Scaling", mod = op + "/Modulation";
        const int base = (6 - n) * 21; // operator 6 comes first in the voice
        const uint32_t id = 1000 * n;
        auto opByte = [&](uint32_t k, const std::string &module, std::string name, double min, double max, int b, int offset = 0,
                          Text text = Text::Number, const char *const *choices = nullptr) {
            p.push_back({ id + k, module, std::move(name), min, max, (double)initVoice[base + b] - offset, Kind::Byte, base + b, offset, text, choices });
        };
        for (int i = 0; i < 4; i++) {
            opByte(1 + i, env, "Rate " + std::to_string(i + 1), 0, 99, i);
            opByte(5 + i, env, "Level " + std::to_string(i + 1), 0, 99, 4 + i);
        }
        opByte(9, scale, "Break Point", 0, 99, 8, 0, Text::Note);
        opByte(10, scale, "Left Depth", 0, 99, 9);
        opByte(11, scale, "Right Depth", 0, 99, 10);
        opByte(12, scale, "Left Curve", 0, 3, 11, 0, Text::Choice, curves);
        opByte(13, scale, "Right Curve", 0, 3, 12, 0, Text::Choice, curves);
        opByte(14, scale, "Rate Scaling", 0, 7, 13);
        opByte(15, mod, "Amp Mod Sensitivity", 0, 3, 14);
        opByte(16, mod, "Velocity Sensitivity", 0, 7, 15);
        opByte(17, op, "Level", 0, 99, 16);
        opByte(18, freq, "Mode", 0, 1, 17, 0, Text::Choice, modes);
        opByte(19, freq, "Coarse", 0, 31, 18);
        opByte(20, freq, "Fine", 0, 99, 19);
        opByte(21, freq, "Detune", -7, 7, 20, 7, Text::Signed);
        p.push_back({ id + 22, op, "Switch", 0, 1, 1, Kind::OpSwitch, 6 - n, 0, Text::Switch, offOn });
    }
    std::sort(p.begin(), p.end(), [](const Param &a, const Param &b) { return a.id < b.id; });
    return p;
}

const std::vector<Param> &params() {
    static const std::vector<Param> table = build();
    return table;
}

int indexOf(uint32_t id) {
    const auto &p = params();
    auto it = std::lower_bound(p.begin(), p.end(), id, [](const Param &a, uint32_t i) { return a.id < i; });
    return it != p.end() && it->id == id ? (int)(it - p.begin()) : -1;
}

// ---- profile conversion ----

namespace {

// Each DX7 algorithm as an Extended algorithm: the Extended operator that plays DX7 operator
// 1-6 (the graphs match on those six; the other two operators are left silent). Found by a
// search over the 88 graphs, preferring DX7 operator n -> Extended operator 9 - n.
struct AlgorithmMap { int algorithm; int op[6]; };
const AlgorithmMap dx7ToExt[32] = {
    { 9, { 8, 7, 6, 5, 4, 3 } }, { 10, { 8, 7, 6, 5, 4, 3 } }, { 11, { 8, 7, 6, 5, 4, 3 } },
    { 12, { 8, 7, 6, 5, 4, 3 } }, { 13, { 8, 7, 6, 5, 4, 3 } }, { 14, { 8, 7, 6, 5, 4, 3 } },
    { 15, { 8, 7, 6, 5, 4, 3 } }, { 16, { 8, 7, 6, 5, 4, 3 } }, { 17, { 8, 7, 6, 5, 4, 3 } },
    { 18, { 5, 4, 3, 8, 6, 7 } }, { 19, { 5, 4, 3, 8, 6, 7 } }, { 20, { 4, 3, 8, 5, 6, 7 } },
    { 21, { 4, 3, 8, 6, 7, 5 } }, { 22, { 8, 7, 6, 5, 4, 3 } }, { 23, { 8, 7, 6, 5, 4, 3 } },
    { 24, { 8, 7, 6, 5, 4, 3 } }, { 25, { 8, 7, 6, 5, 4, 3 } }, { 26, { 8, 7, 6, 5, 4, 3 } },
    { 27, { 8, 7, 6, 5, 4, 3 } }, { 75, { 2, 3, 1, 8, 5, 7 } }, { 29, { 8, 7, 6, 5, 4, 3 } },
    { 30, { 8, 7, 6, 5, 4, 3 } }, { 27, { 1, 8, 7, 4, 5, 3 } }, { 7, { 5, 6, 2, 3, 4, 1 } },
    { 7, { 5, 6, 7, 2, 3, 1 } }, { 16, { 1, 8, 7, 6, 4, 5 } }, { 28, { 8, 7, 6, 5, 4, 3 } },
    { 11, { 8, 7, 5, 4, 3, 1 } }, { 6, { 5, 6, 4, 3, 2, 1 } }, { 3, { 5, 6, 4, 3, 1, 7 } },
    { 4, { 5, 6, 7, 8, 4, 1 } }, { 1, { 2, 3, 4, 5, 6, 1 } },
};

// DX7 (Dexed) envelope rate 0-99 as ms per dB of a decay (Env::advance at 44.1 kHz, no
// rate scaling).
double classicMsPerDb(int rate) {
    const int q = std::min(63, (rate * 41) >> 6);
    const double inc = (double)((4 + (q & 3)) << (2 + 6 + (q >> 2)));
    const double dbPerSecond = inc * (44100.0 / 64) / 16777216.0 * 6.020599913279624;
    return 1000.0 / dbPerSecond;
}

// DX7 ratio-mode frequency factor and detune in cents (at note 60, Dx7Note::osc_freq).
double classicRatio(int coarse, int fine) { return (coarse == 0 ? 0.5 : coarse) * (1 + 0.01 * fine); }
double classicDetuneCents(int detune) {
    const double logfreq = std::log2(261.6255653) ; // octaves
    return 0.0209 * std::exp(-0.396 * logfreq) / 7 * logfreq * (detune - 7) * 1200.0;
}
double classicFixedHz(int coarse, int fine) {
    return std::pow(2.0, (double)((4458616 * ((coarse & 3) * 100 + fine)) >> 3) / 16777216.0);
}
const int ampSensToOffset[4] = { 0, 2, 4, 7 }; // shortcut: an approximate mapping, not measured

template <class F> int closest(int lo, int hi, F error) {
    int best = lo;
    for (int v = lo; v <= hi; v++)
        if (error(v) < error(best))
            best = v;
    return best;
}

double logError(double a, double b) { return std::fabs(std::log(a / b)); }

struct Values {
    std::vector<double> &v;
    double get(uint32_t id) const { return v[(size_t)indexOf(id)]; }
    int geti(uint32_t id) const { return (int)std::lround(get(id)); }
    void set(uint32_t id, double x) { v[(size_t)indexOf(id)] = x; }
};

} // namespace

void convertToExtended(std::vector<double> &values) {
    Values x { values };
    // start from the Extended defaults, every operator silent
    for (const Param &p : params())
        if (p.kind == Kind::Ext)
            values[(size_t)indexOf(p.id)] = p.def;
    for (int n = 1; n <= ext::Ops; n++)
        x.set(1000 * n + 51, 0);
    x.set(5, ext::Extended);
    const AlgorithmMap &m = dx7ToExt[std::clamp(x.geti(1), 1, 32) - 1];
    x.set(40, m.algorithm);
    x.set(41, x.geti(2));
    x.set(42, x.geti(15));
    x.set(45, x.geti(11));
    x.set(46, x.geti(14));
    x.set(47, x.geti(12));
    x.set(48, x.geti(13));
    const double hz = Lfo::classicHz(x.geti(10));
    x.set(49, 0);
    x.set(43, closest(0, 99, [&](int s) { return logError(extLfoHz(s), hz); }));
    const int keySync = x.geti(3), pitchSens = x.geti(16);
    for (int d = 1; d <= 6; d++) {
        const uint32_t c = 1000 * d, e = 1000 * m.op[d - 1];
        x.set(e + 30, keySync);
        const int mode = x.geti(c + 18), coarse = x.geti(c + 19), fine = x.geti(c + 20), detune = x.geti(c + 21) + 7;
        x.set(e + 31, mode);
        if (mode == 0) {
            x.set(e + 32, coarse);
            const double r = classicRatio(coarse, fine);
            x.set(e + 33, closest(0, 127, [&](int f) { return logError(extRatio(coarse, f), r); }));
        } else {
            const double f = classicFixedHz(coarse, fine);
            int bc = 0, bf = 0;
            for (int cc = 0; cc < 32; cc++)
                for (int ff = 0; ff < 128; ff++)
                    if (logError(extFixedHz(cc, ff), f) < logError(extFixedHz(bc, bf), f)) {
                        bc = cc;
                        bf = ff;
                    }
            x.set(e + 32, bc);
            x.set(e + 33, bf);
        }
        const double cents = classicDetuneCents(detune);
        x.set(e + 34, closest(-15, 15, [&](int t) { return std::fabs(extDetuneCents(t, 60) - cents); }));
        for (int i = 0; i < 4; i++) {
            x.set(e + 41 + i, x.geti(c + 5 + i));
            const double ms = classicMsPerDb(x.geti(c + 1 + i));
            x.set(e + 45 + i, closest(0, 99, [&](int t) {
                return logError(i == 0 ? extAttackMsPerDb(t) : extEgMsPerDb(t), ms);
            }));
        }
        x.set(e + 50, x.geti(c + 14));
        x.set(e + 51, x.geti(c + 22) ? x.geti(c + 17) : 0);
        for (int k = 0; k < 5; k++)
            x.set(e + 52 + k, x.geti(c + 9 + k));
        x.set(e + 57, x.geti(c + 16));
        x.set(e + 58, pitchSens);
        x.set(e + 59, ampSensToOffset[std::clamp(x.geti(c + 15), 0, 3)]);
    }
}

bool convertToClassic(std::vector<double> &values) {
    std::vector<double> out = values;
    Values x { values }, y { out };
    const int alg = x.geti(40);
    int dx = -1;
    for (int a = 0; a < 32 && dx < 0; a++) {
        if (dx7ToExt[a].algorithm != alg)
            continue;
        bool silent = true; // the operators the DX7 graph leaves out must be silent
        for (int n = 1; n <= ext::Ops; n++)
            if (std::find(dx7ToExt[a].op, dx7ToExt[a].op + 6, n) == dx7ToExt[a].op + 6 && x.geti(1000 * n + 51) != 0)
                silent = false;
        if (silent)
            dx = a;
    }
    if (dx < 0)
        return false;
    const AlgorithmMap &m = dx7ToExt[dx];
    const int keySync = x.geti(1000 * m.op[0] + 30), pitchSens = x.geti(1000 * m.op[0] + 58);
    if (x.geti(44) != 0 || x.geti(49) != 0)
        return false; // LFO phase and the Extended speed range have no Classic equivalent
    for (int d = 1; d <= 6; d++) {
        const uint32_t e = 1000 * m.op[d - 1];
        if (x.geti(e + 30) != keySync || x.geti(e + 58) != pitchSens)
            return false; // one value for the whole voice in Classic
        if (x.geti(e + 49) != 0 || x.geti(e + 37) != 0 || x.geti(e + 38) != 0 || x.geti(e + 36) != 0 ||
            x.geti(e + 35) != 0 || x.geti(e + 57) < 0 || x.geti(e + 60) != 0)
            return false; // hold, operator pitch EG, pitch velocity, key follow, negative velocity, wave
        if (std::find(ampSensToOffset, ampSensToOffset + 4, x.geti(e + 59)) == ampSensToOffset + 4)
            return false;
    }
    y.set(5, ext::Classic);
    y.set(1, dx + 1);
    y.set(2, x.geti(41));
    y.set(3, keySync);
    y.set(15, x.geti(42));
    y.set(11, x.geti(45));
    y.set(14, x.geti(46));
    y.set(12, x.geti(47));
    y.set(13, x.geti(48));
    const double hz = extLfoHz(x.geti(43));
    y.set(10, closest(0, 99, [&](int s) { return logError(Lfo::classicHz(s), hz); }));
    y.set(16, pitchSens);
    for (int d = 1; d <= 6; d++) {
        const uint32_t c = 1000 * d, e = 1000 * m.op[d - 1];
        const int mode = x.geti(e + 31), coarse = x.geti(e + 32), fine = x.geti(e + 33);
        y.set(c + 18, mode);
        if (mode == 0) {
            y.set(c + 19, coarse);
            const double r = extRatio(coarse, fine);
            y.set(c + 20, closest(0, 99, [&](int f) { return logError(classicRatio(coarse, f), r); }));
        } else {
            const double f = extFixedHz(coarse, fine);
            int bc = 0, bf = 0;
            for (int cc = 0; cc < 4; cc++)
                for (int ff = 0; ff < 100; ff++)
                    if (logError(classicFixedHz(cc, ff), f) < logError(classicFixedHz(bc, bf), f)) {
                        bc = cc;
                        bf = ff;
                    }
            y.set(c + 19, bc);
            y.set(c + 20, bf);
        }
        const double cents = extDetuneCents(x.geti(e + 34), 60);
        y.set(c + 21, closest(0, 14, [&](int t) { return std::fabs(classicDetuneCents(t) - cents); }) - 7);
        for (int i = 0; i < 4; i++) {
            y.set(c + 5 + i, x.geti(e + 41 + i));
            const double ms = i == 0 ? extAttackMsPerDb(x.geti(e + 45 + i)) : extEgMsPerDb(x.geti(e + 45 + i));
            y.set(c + 1 + i, closest(0, 99, [&](int r) { return logError(classicMsPerDb(r), ms); }));
        }
        y.set(c + 14, x.geti(e + 50));
        y.set(c + 17, x.geti(e + 51));
        for (int k = 0; k < 5; k++)
            y.set(c + 9 + k, x.geti(e + 52 + k));
        y.set(c + 16, x.geti(e + 57));
        y.set(c + 15, (double)(std::find(ampSensToOffset, ampSensToOffset + 4, x.geti(e + 59)) - ampSensToOffset));
        y.set(c + 22, 1);
    }
    values = out;
    return true;
}

} // namespace oscairn_fm
