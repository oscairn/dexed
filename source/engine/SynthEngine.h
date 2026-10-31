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

#include <atomic>
#include <memory>
#include <vector>

#include "msfa/controllers.h"
#include "msfa/dx7note.h"
#include "msfa/lfo.h"
#include "msfa/synth_rate.h"
#include "msfa/synth.h"
#include "msfa/fm_core.h"
#include "msfa/tuning.h"
#include "engine/EngineMkI.h"
#include "engine/EngineOpl.h"
#include "PluginFx.h"

// Which host note a voice plays, for CLAP note IDs and NOTE_END. -1 is "any" or "none".
struct NoteRef {
    int32_t id = -1;
    int16_t port = -1, channel = -1, key = -1;
};

struct ProcessorVoice {
    int channel = 0;
    int midi_note = -1;
    int velocity = 0;
    bool keydown = false;
    bool sustained = false;
    bool live = false;
    int32_t keydown_seq = -1;

    int mpePitchBend = 8192;
    Dx7Note *dx7_note = nullptr;

    NoteRef ref;             // the note that started this voice
    bool noteActive = false; // the host has not been told the note ended
    int context = 0;         // the sound (SynthContext) the voice plays
};

enum DexedEngineResolution {
    DEXED_ENGINE_MODERN,
    DEXED_ENGINE_MARKI,
    DEXED_ENGINE_OPL
};

// One prepared sound (RFC 0005 sound context): its voice data and settings, its LFO (the DX7
// had one per instrument) and its output filter. Context 0 is the default sound.
struct SynthContext {
    const uint8_t *patch = nullptr; // 156 bytes, owned by the engine's user
    Controllers ctrl;               // settings and controller values
    Lfo lfo;
    PluginFx fx;
    bool monoMode = false;
    int engineType = -1;
    int lastActiveVoice = 0;

    // audio thread
    float extra_buf[N];        // rendered ahead (see SynthEngine::render)
    std::vector<float> block;  // contexts other than 0 render a block here before mixing
    bool rendering = false;    // rendered in the current block
    bool tail = false;         // the last block was not silent
    uint16_t selectedChannels = 0;

    // published by publishContextState() for the main thread
    std::atomic<uint32_t> activeVoices { 0 };
    std::atomic<uint32_t> selectedMask { 0 };
    std::atomic<bool> silent { true }; // no voices, not selected, no tail
};

// Chooses the voice for a new note. One place, so the policy can be replaced without
// touching context handling.
// shortcut: Dexed's policy (+4 not playing, +2 key up, +1 same pitch, oldest key-down on ties)
// across the whole shared pool, as a placeholder; pending the voice-stealing decision with
// the main Oscairn agent (docs/oscairn-plan.md, section 10, question 4).
struct VoiceStealing {
    static int choose(const ProcessorVoice *voices, int count, int currentNote, int pitch);
};

class SynthEngine {
public:
    static const int MAX_ACTIVE_NOTES = 16;
    static const int MAX_CONTEXTS = 8;

    // Supplies the events of the block being rendered, in order.
    struct EventSource {
        virtual ~EventSource() = default;
        // Apply every pending event whose sample position is at or before samplePos.
        virtual void dispatchUntil(int samplePos) = 0;
    };

    // Told when a voice's note has ended (finished sounding, stolen or choked).
    struct NoteEndSink {
        virtual ~NoteEndSink() = default;
        virtual void noteEnded(const NoteRef &note) = 0;
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

    // The 156-byte voice (155 DX7 bytes and the operator switch byte) of context 0 (or c).
    // Owned by the caller and read while voices play.
    void setPatch(const uint8_t *patch) { contexts[0].patch = patch; }
    void setPatch(int c, const uint8_t *patch) { contexts[c].patch = patch; }
    void setTuning(std::shared_ptr<TuningState> tuning);
    void setMtsClient(MTSClient *client) { mtsClient = client; }

    // maxFrames: the largest block render() will get; contexts other than 0 can only play
    // when it is given.
    void prepare(double sampleRate, int maxFrames = 0);
    void release();

    // Renders numSamples mono samples into out (overwriting it), dispatching events on the
    // 64-sample grid as it goes, then applies the output filter.
    void render(float *out, int numSamples, EventSource &events);

    // A short MIDI channel message (not SysEx). channel is 1-16.
    MidiResult processMidiMessage(const uint8_t *buf, int channel);

    void keydown(uint8_t chan, uint8_t pitch, uint8_t velo);
    void keydown(uint8_t chan, uint8_t pitch, uint8_t velo, const NoteRef &ref);
    void keyup(uint8_t chan, uint8_t pitch, uint8_t velo);
    void panic();

    // CLAP note-off and choke: every held (note-off) or sounding (choke) voice whose note
    // matches; a note ID >= 0 matches by ID alone, otherwise -1 fields match anything.
    void releaseNotes(const NoteRef &match);
    void chokeNotes(const NoteRef &match);
    // Reports, through the sink, every note whose voice is released and no longer sounding.
    void collectEndedNotes();
    void setNoteEndSink(NoteEndSink *sink) { noteEndSink = sink; }

    // Re-reads the patch into the playing voices and the LFO (of context 0, or c).
    void refreshVoices() { refreshVoices(0); }
    void refreshVoices(int c);
    void resetLfo() { resetLfo(0); }
    void resetLfo(int c);
    // Stops every voice of context c (no release).
    void panicContext(int c);

    bool peekVoiceStatus(VoiceStatus &voiceStatus);
    int tuningTranspositionShift() { return tuningTranspositionShift(0); }
    int tuningTranspositionShift(int c);

    int getEngineType() { return contexts[0].engineType; }
    void setEngineType(int tp) { setEngineType(0, tp); }
    void setEngineType(int c, int tp);
    void setMonoMode(bool mono) { contexts[0].monoMode = mono; }
    void setMonoMode(int c, bool mono) { contexts[c].monoMode = mono; }

    // Contexts. selectContext is for the audio thread, between blocks; channel -1 is all.
    SynthContext &context(int c) { return contexts[c]; }
    void selectContext(int c, int channel);
    int selectedContext(int channel) const { return selected[channel]; }
    // After a block (and collectEndedNotes): publishes voice counts, selections and silence.
    void publishContextState();

    SynthContext contexts[MAX_CONTEXTS];

    // Context 0's controllers and output filter (the single sound of the desktop plugin).
    // in MIDI units (0x4000 is neutral)
    Controllers &controllers { contexts[0].ctrl };
    PluginFx &fx { contexts[0].fx };
    bool normalizeDxVelocity = false;

private:
    ProcessorVoice voices[MAX_ACTIVE_NOTES];
    int currentNote;

    bool sustain;
    int selected[16] = {}; // the context each MIDI channel plays

    // Extra buffering for when GetSamples wants a buffer not a multiple of N (the samples
    // are per context; the count is shared, so every context keeps the same 64-sample grid)
    int extra_buf_size;

    FmCore engineMsfa;
    EngineMkI engineMkI;
    EngineOpl engineOpl;

    SynthRate rate;
    std::shared_ptr<TuningState> tuningState;
    MTSClient *mtsClient = nullptr;

    int32_t nextKeydownSeq;

    NoteEndSink *noteEndSink = nullptr;
    void keyupVoice(int note);
    void endNote(int note);
};
