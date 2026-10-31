/*
 * Octet parameter table.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * IDs are explicit and never change between versions (tests/oscairn_params.cpp checks a
 * fingerprint of the table). Voice parameters: 1-99. Plugin settings: 100-199. Operator n:
 * 1000 * n + 1..99, so operators 7 and 8 can be added without moving anything.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oscairn_fm {

enum class Kind {
    Byte,     // a byte of the DX7 voice: byte = value + offset
    OpSwitch, // a bit of the operator switch byte (155)
    Setting   // a plugin setting (see Setting)
};

enum class Setting {
    None, Engine, Mono, PortamentoTime, Glissando, PitchBendUp, PitchBendDown, PitchBendStep,
    WheelRange, WheelPitch, WheelAmp, WheelEg, FootRange, FootPitch, FootAmp, FootEg,
    BreathRange, BreathPitch, BreathAmp, BreathEg, AftertouchRange, AftertouchPitch, AftertouchAmp,
    AftertouchEg, MasterTune, Cutoff, Resonance, Output
};

enum class Text { Number, Signed, Choice, Note, Switch, Percent, Tune };

struct Param {
    uint32_t id;
    std::string module, name;
    double min, max, def;
    Kind kind;
    int target;      // Byte: voice offset; OpSwitch: bit; Setting: Setting
    int offset = 0;  // Byte: added to the value to give the byte
    Text text = Text::Number;
    const char *const *choices = nullptr;
    bool stepped() const { return text != Text::Percent && text != Text::Tune; }
};

// The table, sorted by ID. Built on first use (not on the audio thread).
const std::vector<Param> &params();
// Index in params() of an ID, or -1.
int indexOf(uint32_t id);

// The DX7 initial voice ("INIT VOICE"), 155 bytes plus the operator switch byte.
extern const uint8_t initVoice[156];

} // namespace oscairn_fm
