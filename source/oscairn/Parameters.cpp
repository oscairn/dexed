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

} // namespace oscairn_fm
