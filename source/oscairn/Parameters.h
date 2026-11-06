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
 * fingerprint of the table). Voice parameters: 1-99 and 200-299. Plugin settings: 100-199. Operator n:
 * 1000 * n + 1..99, so operators 7 and 8 can be added without moving anything.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "msfa/ext_voice.h"

namespace oscairn_fm {

enum class Kind {
    Byte,     // a byte of the DX7 voice: byte = value + offset
    OpSwitch, // a bit of the operator switch byte (155)
    Setting,  // a plugin setting (see Setting)
    Ext       // an Extended profile value: ExtendedVoice::v[target] = value
};

enum class Setting {
    None, Engine, Mono, PortamentoTime, Glissando, PitchBendUp, PitchBendDown, PitchBendStep,
    WheelRange, WheelPitch, WheelAmp, WheelEg, FootRange, FootPitch, FootAmp, FootEg,
    BreathRange, BreathPitch, BreathAmp, BreathEg, AftertouchRange, AftertouchPitch, AftertouchAmp,
    AftertouchEg, MasterTune, Cutoff, Resonance, Output, OutputBus
};

enum class Text { Number, Signed, Choice, Note, Switch, Percent, Tune, Key };

struct Param {
    uint32_t id;
    std::string module, name;
    double min, max, def;
    Kind kind;
    int target;      // Byte: voice offset; OpSwitch: bit; Setting: Setting; Ext: index
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

// The Extended values of a new sound: the documented defaults, profile Classic. The
// Extended init sound is the same with the profile set to Extended.
const ExtendedVoice &extDefaults();

// "Convert to Extended" (approximate where the tables differ): Classic values to the same
// sound's Extended values, profile Extended. values are by params() index.
void convertToExtended(std::vector<double> &values);
// Back to Classic: only when the Extended values stay within what Classic can express;
// otherwise false and values are unchanged.
bool convertToClassic(std::vector<double> &values);

} // namespace oscairn_fm
