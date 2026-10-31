/**
 *
 * Copyright (c) 2013-2025 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#pragma once

// The headless synth engine: voice allocation, MIDI handling and rendering, moved out of
// DexedAudioProcessor unchanged so the plugin, the tests and (later) a CLAP direct-process
// path share it. No JUCE.

#include <memory>

#include "msfa/controllers.h"
#include "msfa/dx7note.h"
#include "msfa/lfo.h"
#include "msfa/synth.h"
#include "msfa/fm_core.h"
#include "msfa/tuning.h"
#include "engine/EngineMkI.h"
#include "engine/EngineOpl.h"
#include "PluginFx.h"

struct ProcessorVoice {
    int channel;
    int midi_note;
    int velocity;
    bool keydown;
    bool sustained;
    bool live;
    int32_t keydown_seq;

    int mpePitchBend;
    Dx7Note *dx7_note;
};

enum DexedEngineResolution {
    DEXED_ENGINE_MODERN,
    DEXED_ENGINE_MARKI,
    DEXED_ENGINE_OPL
};

class SynthEngine {
public:
    static const int MAX_ACTIVE_NOTES = 16;

    // Supplies the events of the block being rendered, in order.
    struct EventSource {
        virtual ~EventSource() = default;
        // Apply every pending event whose sample position is at or before samplePos.
        virtual void dispatchUntil(int samplePos) = 0;
    };

    // What processMidiMessage left for the caller to do.
    enum class MidiResult {
        Handled,
        ProgramChange,          // 0xC0: the caller selects program buf[1]
        UnhandledControlChange, // a CC the engine does not use: the caller may map it
        Panic                   // CC 120: the engine panicked; the caller resets its own note state
    };

    SynthEngine();
    ~SynthEngine();

    // The 156-byte voice (155 DX7 bytes and the operator switch byte). Owned by the caller
    // and read while voices play.
    void setPatch(const uint8_t *patch) { this->patch = patch; }
    void setTuning(std::shared_ptr<TuningState> tuning);
    void setMtsClient(MTSClient *client) { mtsClient = client; }

    void prepare(double sampleRate);
    void release();

    // Renders numSamples mono samples into out (overwriting it), dispatching events on the
    // 64-sample grid as it goes, then applies the output filter.
    void render(float *out, int numSamples, EventSource &events);

    // A short MIDI channel message (not SysEx). channel is 1-16.
    MidiResult processMidiMessage(const uint8_t *buf, int channel);

    void keydown(uint8_t chan, uint8_t pitch, uint8_t velo);
    void keyup(uint8_t chan, uint8_t pitch, uint8_t velo);
    void panic();

    // Re-reads the patch into the playing voices and the LFO.
    void refreshVoices();
    void resetLfo();

    bool peekVoiceStatus(VoiceStatus &voiceStatus);
    int tuningTranspositionShift();

    int getEngineType();
    void setEngineType(int tp);
    void setMonoMode(bool mono) { monoMode = mono; }

    // in MIDI units (0x4000 is neutral)
    Controllers controllers;
    PluginFx fx;
    bool normalizeDxVelocity = false;

private:
    ProcessorVoice voices[MAX_ACTIVE_NOTES];
    int currentNote;

    // The original DX7 had one single LFO. Later units had an LFO per note.
    Lfo lfo;

    bool sustain;
    bool monoMode;

    // Extra buffering for when GetSamples wants a buffer not a multiple of N
    float extra_buf[N];
    int extra_buf_size;

    uint32_t engineType;
    FmCore engineMsfa;
    EngineMkI engineMkI;
    EngineOpl engineOpl;

    const uint8_t *patch = nullptr;
    std::shared_ptr<TuningState> tuningState;
    MTSClient *mtsClient = nullptr;

    int lastActiveVoice = 0;
    int32_t nextKeydownSeq;
};
