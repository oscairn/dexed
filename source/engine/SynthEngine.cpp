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

// Moved from PluginProcessor.cpp, midi/PluginMidi.cpp and Tuning.cpp without behaviour change.

#include "SynthEngine.h"

#include "msfa/freqlut.h"
#include "msfa/sin.h"
#include "msfa/exp2.h"
#include "msfa/env.h"
#include "msfa/pitchenv.h"
#include "msfa/porta.h"
#include "msfa/aligned_buf.h"
#include "msfa/fm_op_kernel.h"

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#endif

namespace {
    // Flush denormals to zero while rendering, with the same bits as juce::ScopedNoDenormals,
    // which the plugin always rendered under: the output filter's decay otherwise produces
    // denormals that change the output and cost CPU.
    struct ScopedFlushDenormals {
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
        unsigned int saved = _mm_getcsr();
        ScopedFlushDenormals() { _mm_setcsr(saved | 0x8040); }
        ~ScopedFlushDenormals() { _mm_setcsr(saved); }
#elif defined(__aarch64__) && !defined(_MSC_VER)
        intptr_t saved;
        ScopedFlushDenormals() {
            asm volatile("mrs %0, fpcr" : "=r"(saved));
            asm volatile("msr fpcr, %0" : : "ri"(saved | (1 << 24)));
        }
        ~ScopedFlushDenormals() { asm volatile("msr fpcr, %0" : : "ri"(saved)); }
#else
        // shortcut: no flush-to-zero on other targets (32-bit ARM, MSVC ARM64); add it if one is built
#endif
    };
}

// DX7 voice byte offsets
static const int OSC_KEY_SYNC = 136;
static const int LFO_PARAMS = 137;
static const int TRANSPOSE = 144;

int VoiceStealing::choose(const ProcessorVoice *voices, int count, int currentNote, int pitch) {
    // Stealing preference (highest score wins; ties broken by least-recent keydown):
    //   +4 not playing, +2 key up, +1 same pitch.
    int bestNote   = currentNote;
    int bestScore  = -1;
    int note       = currentNote;
    for (int i = 0; i < count; i++) {
        const ProcessorVoice &v = voices[note];
        int score = 0;
        if ( !v.dx7_note->isPlaying() ) score += 4;
        if ( !v.keydown )               score += 2;
        if (  v.midi_note == pitch )    score += 1;
        if ( score > bestScore ||
             (score == bestScore && v.keydown_seq < voices[bestNote].keydown_seq) ) {
            bestNote  = note;
            bestScore = score;
        }
        note = (note + 1) % count;
    }
    return bestNote;
}

SynthEngine::SynthEngine() {
    // sample-rate independent tables shared by every instance: built once, thread-safely
    static const bool sharedTablesReady = [] {
        Exp2::init();
        Tanh::init();
        Sin::init();
        return true;
    }();
    (void)sharedTablesReady;

    currentNote = -1;

    for (int c = 0; c < MAX_CONTEXTS; c++) {
        SynthContext &x = contexts[c];
        setEngineType(c, DEXED_ENGINE_MARKI);
        x.ctrl.pitch_range_up = 3;
        x.ctrl.pitch_range_dn = 3;
        x.ctrl.pitch_step = 0;
        x.ctrl.masterTune = 0;
    }
    contexts[0].selectedChannels = 0xffff;

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        voices[note].dx7_note = NULL;
    }
}

SynthEngine::~SynthEngine() {
    release();
}

void SynthEngine::setTuning(std::shared_ptr<TuningState> t) {
    tuningState = t;
    for( int i=0; i<MAX_ACTIVE_NOTES; ++i )
        if( voices[i].dx7_note != nullptr )
            voices[i].dx7_note->tuning_state_ = tuningState;
}

void SynthEngine::prepare(double sampleRate, int maxFrames) {
    rate.init(sampleRate);
    for (int c = 0; c < MAX_CONTEXTS; c++) {
        contexts[c].lfo.init(sampleRate);
        contexts[c].fx.init(sampleRate);
        if ( c > 0 )
            contexts[c].block.assign((size_t)(maxFrames > 0 ? maxFrames : 0), 0.0f);
    }

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        voices[note].dx7_note = new Dx7Note(tuningState, mtsClient, &rate);
        voices[note].midi_note = -1;
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
        voices[note].keydown_seq = -1;
        voices[note].noteActive = false;
        voices[note].context = 0;
    }

    currentNote = 0;
    nextKeydownSeq = 0;
    for (int c = 0; c < MAX_CONTEXTS; c++) {
        Controllers &ctrl = contexts[c].ctrl;
        ctrl.pitch_cc = 0x2000;
        ctrl.modwheel_cc = 0;
        ctrl.foot_cc = 0;
        ctrl.breath_cc = 0;
        ctrl.aftertouch_cc = 0;
        ctrl.portamento_enable_cc = false;
        ctrl.portamento_cc = 0;
        ctrl.refresh();
        contexts[c].rendering = false;
        contexts[c].tail = false;
    }

    sustain = false;
    extra_buf_size = 0;

    for (int c = 0; c < MAX_CONTEXTS; c++)
        if ( contexts[c].patch != nullptr )
            contexts[c].lfo.reset(contexts[c].patch + LFO_PARAMS);
}

void SynthEngine::release() {
    currentNote = -1;

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        if ( voices[note].dx7_note != NULL ) {
            delete voices[note].dx7_note;
            voices[note].dx7_note = NULL;
        }
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
        voices[note].noteActive = false;
    }
}

void SynthEngine::refreshVoices(int c) {
    SynthContext &x = contexts[c];
    for(int i=0;i < MAX_ACTIVE_NOTES;i++) {
        if ( voices[i].live && voices[i].context == c )
            voices[i].dx7_note->update(x.patch, voices[i].midi_note, voices[i].velocity, voices[i].channel);
    }
    x.lfo.reset(x.patch + LFO_PARAMS);
}

void SynthEngine::resetLfo(int c) {
    contexts[c].lfo.reset(contexts[c].patch + LFO_PARAMS);
}

void SynthEngine::panicContext(int c) {
    for(int i=0;i<MAX_ACTIVE_NOTES;i++) {
        if ( voices[i].context != c )
            continue;
        voices[i].midi_note = -1;
        voices[i].keydown = false;
        voices[i].live = false;
        if ( voices[i].dx7_note != NULL ) {
            voices[i].dx7_note->oscSync();
        }
    }
}

void SynthEngine::selectContext(int c, int channel) {
    for (int ch = 0; ch < 16; ch++) {
        if ( channel >= 0 && ch != channel )
            continue;
        contexts[selected[ch]].selectedChannels &= (uint16_t)~(1u << ch);
        selected[ch] = c;
        contexts[c].selectedChannels |= (uint16_t)(1u << ch);
    }
}

void SynthEngine::publishContextState() {
    uint32_t count[MAX_CONTEXTS] = {};
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
        if ( voices[i].noteActive )
            count[voices[i].context]++;
    for (int c = 0; c < MAX_CONTEXTS; c++) {
        SynthContext &x = contexts[c];
        x.activeVoices.store(count[c], std::memory_order_relaxed);
        x.selectedMask.store(x.selectedChannels, std::memory_order_relaxed);
        x.silent.store(count[c] == 0 && x.selectedChannels == 0 && !x.tail, std::memory_order_release);
    }
}

void SynthEngine::render(float *channelData, int numSamples, EventSource &events) {
    ScopedFlushDenormals noDenormals;
    int i;

    // Contexts that play this block: the default one (as the single-sound engine always
    // did), and any other that is selected, has voices or has not yet gone silent.
    bool active[MAX_CONTEXTS] = { true };
    float *outputs[MAX_CONTEXTS] = { channelData };
    int activeCount = 1;
    bool hasVoice[MAX_CONTEXTS] = {};
    for (int n = 0; n < MAX_ACTIVE_NOTES; n++)
        if ( voices[n].live || voices[n].noteActive )
            hasVoice[voices[n].context] = true;
    for (int c = 1; c < MAX_CONTEXTS; c++) {
        SynthContext &x = contexts[c];
        if ( (int)x.block.size() < numSamples || !(x.selectedChannels || hasVoice[c] || x.tail) ) {
            x.rendering = false;
            continue;
        }
        if ( !x.rendering ) // starting again: nothing rendered ahead
            for (int j = 0; j < N; j++)
                x.extra_buf[j] = 0;
        x.rendering = true;
        active[c] = true;
        outputs[c] = x.block.data();
        activeCount++;
    }

    // flush first events
    for (int c = 0; c < MAX_CONTEXTS; c++) {
        if ( !active[c] )
            continue;
        for (i=0; i < numSamples && i < extra_buf_size; i++) {
            outputs[c][i] = contexts[c].extra_buf[i];
        }
    }
    i = numSamples < extra_buf_size ? numSamples : extra_buf_size;

    // remaining buffer is still to be processed
    if (extra_buf_size > numSamples) {
        for (int c = 0; c < MAX_CONTEXTS; c++) {
            if ( !active[c] )
                continue;
            float *extra_buf = contexts[c].extra_buf;
            for (int j = 0; j < extra_buf_size - numSamples; j++) {
                extra_buf[j] = extra_buf[j + numSamples];
            }
        }
        extra_buf_size -= numSamples;

        // flush the events, they will be process in the next cycle
        events.dispatchUntil(numSamples);
    } else {
        for (; i < numSamples; i += N) {
            AlignedBuf<int32_t, N> audiobuf;
            float sumbuf[MAX_CONTEXTS][N];

            events.dispatchUntil(i);

            for (int j = 0; j < N; ++j) {
                audiobuf.get()[j] = 0;
            }
            int32_t lfovalue[MAX_CONTEXTS], lfodelay[MAX_CONTEXTS];
            for (int c = 0; c < MAX_CONTEXTS; c++) {
                if ( !active[c] )
                    continue;
                for (int j = 0; j < N; ++j)
                    sumbuf[c][j] = 0;
                lfovalue[c] = contexts[c].lfo.getsample();
                lfodelay[c] = contexts[c].lfo.getdelay();
            }

            bool checkMTSESPRetuning = tuningState->is_standard_tuning() &&
                                        MTS_HasMaster(mtsClient);

            for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
                const int c = voices[note].context;
                if (voices[note].live && active[c]) {

                    if (checkMTSESPRetuning)
                        voices[note].dx7_note->updateBasePitches();

                    voices[note].dx7_note->compute(audiobuf.get(), lfovalue[c], lfodelay[c], &contexts[c].ctrl);

                    for (int j=0; j < N; ++j) {
                        int32_t val = audiobuf.get()[j];

                        val = val >> 4;
                        int clip_val = val < -(1 << 24) ? 0x8000 : val >= (1 << 24) ? 0x7fff : val >> 9;
                        float f = ((float) clip_val) / (float) 0x8000;
                        if( f > 1 ) f = 1;
                        if( f < -1 ) f = -1;
                        sumbuf[c][j] += f;
                        audiobuf.get()[j] = 0;
                    }
                }
            }

            int jmax = numSamples - i;
            for (int c = 0; c < MAX_CONTEXTS; c++) {
                if ( !active[c] )
                    continue;
                float *out = outputs[c];
                float *extra_buf = contexts[c].extra_buf;
                for (int j = 0; j < N; ++j) {
                    if (j < jmax) {
                        out[i + j] = sumbuf[c][j];
                    } else {
                        extra_buf[j - jmax] = sumbuf[c][j];
                    }
                }
            }
        }
        extra_buf_size = i - numSamples;
    }

    events.dispatchUntil(numSamples);
    contexts[0].fx.process(channelData, numSamples);

    // Each context through its own output filter, then summed (RFC 0005).
    if ( activeCount > 1 ) {
        for (int c = 1; c < MAX_CONTEXTS; c++) {
            if ( !active[c] )
                continue;
            float *out = outputs[c];
            contexts[c].fx.process(out, numSamples);
            bool sound = false;
            for (int j = 0; j < numSamples; j++) {
                channelData[j] += out[j];
                sound = sound || out[j] != 0;
            }
            for (int j = 0; j < extra_buf_size && !sound; j++)
                sound = contexts[c].extra_buf[j] != 0;
            contexts[c].tail = sound;
        }
    }
}

void SynthEngine::keydown(uint8_t channel, uint8_t pitch, uint8_t velo) {
    NoteRef ref;
    ref.port = 0;
    ref.channel = (int16_t)(channel - 1);
    ref.key = pitch;
    keydown(channel, pitch, velo, ref);
}

void SynthEngine::keydown(uint8_t channel, uint8_t pitch, uint8_t velo, const NoteRef &ref) {
    if ( velo == 0 ) {
        keyup(channel, pitch, velo);
        return;
    }

    const int c = selected[(channel - 1) & 15];
    SynthContext &x = contexts[c];
    const uint8_t *patch = x.patch;

    pitch += tuningTranspositionShift(c);

    if ( normalizeDxVelocity ) {
        velo = ((float)velo) * 0.7874015; // 100/127
    }

    // triggerLfo (any key held in this sound?) + MPE same-channel-keydown detection
    bool triggerLfo = true;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        const ProcessorVoice &v = voices[i];
        if ( v.keydown ) {
            if ( v.context == c )
                triggerLfo = false;
            // Two keydowns on the same channel => source is not MPE.
            if ( contexts[0].ctrl.mpeEnabled && v.channel == channel )
                for (int k = 0; k < MAX_CONTEXTS; k++)
                    contexts[k].ctrl.mpeEnabled = false;
        }
    }

    if ( triggerLfo )
        x.lfo.keydown();

    int note = VoiceStealing::choose(voices, MAX_ACTIVE_NOTES, currentNote, pitch);

    if ( voices[note].noteActive )
        endNote(note); // stolen
    voices[note].ref = ref;
    voices[note].noteActive = true;
    voices[note].context = c;

    currentNote = (note + 1) % MAX_ACTIVE_NOTES;
    voices[note].channel = channel;
    voices[note].midi_note = pitch;
    voices[note].velocity = velo;
    voices[note].sustained = sustain;
    voices[note].keydown = true;
    voices[note].keydown_seq = nextKeydownSeq++;
    // to avoid click, don't sync oscillators when voice stealing
    bool voice_steal = voices[note].dx7_note->isPlaying();
    voices[note].dx7_note->init(patch, pitch, velo, channel, &x.ctrl);
    if ( patch[OSC_KEY_SYNC] && !voice_steal ) {
        voices[note].dx7_note->oscSync();
    }
    if ( (voices[x.lastActiveVoice].midi_note != -1 && x.ctrl.portamento_enable_cc)
       && x.ctrl.portamento_cc > 0 ) {
        voices[note].dx7_note->initPortamento(*voices[x.lastActiveVoice].dx7_note);
    }

    if ( x.monoMode ) {
        for(int i=0; i<MAX_ACTIVE_NOTES; i++) {
            if ( voices[i].live && voices[i].context == c ) {
                // all keys are up, only transfer signal
                if ( ! voices[i].keydown ) {
                    voices[i].live = false;
                    voices[note].dx7_note->transferSignal(*voices[i].dx7_note);
                    break;
                }
                if ( voices[i].midi_note > pitch ) {
                    voices[i].live = false;
                    voices[note].dx7_note->transferState(*voices[i].dx7_note);
                    break;
                }
                return;
            }
        }
    }
    else if ( !patch[OSC_KEY_SYNC] ) {
        // if another note at the same pitch is playing, transfer phase
        // to avoid unpredictable destructive interference. this can cause
        // clicking when voice stealing, but we've tried to choose a voice
        // to steal that will minimise the chances of clicking
        for(int i=0; i<MAX_ACTIVE_NOTES; i++) {
            if ( i != note && voices[i].context == c && voices[i].dx7_note->isPlaying() && voices[i].midi_note == pitch ) {
                voices[note].dx7_note->transferPhase(*voices[i].dx7_note);
                break;
            }
        }
    }

    voices[note].live = true;
    x.lastActiveVoice = note;
}

void SynthEngine::keyup(uint8_t chan, uint8_t pitch, uint8_t velo) {
    int note;
    for (note=0; note<MAX_ACTIVE_NOTES; ++note) {
        // the pitch as the voice's sound transposed it
        if ( voices[note].keydown && // but still only grab the one which is keydown
             ( ( controllers.mpeEnabled && voices[note].channel == chan ) || // MPE node - find voice by channel
               (!controllers.mpeEnabled && voices[note].midi_note == (uint8_t)(pitch + tuningTranspositionShift(voices[note].context)) ) ) ) { // regular mode find voice by pitch
            voices[note].keydown = false;
            break;
        }
    }

    // note not found ?
    if ( note >= MAX_ACTIVE_NOTES ) {
        return;
    }

    keyupVoice(note);
}

void SynthEngine::keyupVoice(int note) {
    const int c = voices[note].context;
    if ( contexts[c].monoMode ) {
        int highNote = -1;
        int target = 0;
        for (int i=0; i<MAX_ACTIVE_NOTES;i++) {
            if ( voices[i].keydown && voices[i].context == c && voices[i].midi_note > highNote ) {
                target = i;
                highNote = voices[i].midi_note;
            }
        }

        if ( highNote != -1 && voices[note].live ) {
            voices[note].live = false;
            voices[target].live = true;
            voices[target].dx7_note->transferState(*voices[note].dx7_note);
        }
    }

    if ( sustain ) {
        voices[note].sustained = true;
    } else {
        voices[note].dx7_note->keyup();
    }
}

static bool matches(const NoteRef &voice, const NoteRef &match) {
    if ( match.id >= 0 )
        return voice.id == match.id;
    return (match.port < 0 || match.port == voice.port) &&
           (match.channel < 0 || match.channel == voice.channel) &&
           (match.key < 0 || match.key == voice.key);
}

void SynthEngine::releaseNotes(const NoteRef &match) {
    for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
        if ( voices[note].keydown && voices[note].noteActive && matches(voices[note].ref, match) ) {
            voices[note].keydown = false;
            keyupVoice(note);
        }
    }
}

void SynthEngine::chokeNotes(const NoteRef &match) {
    for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
        if ( voices[note].noteActive && matches(voices[note].ref, match) ) {
            voices[note].keydown = false;
            voices[note].sustained = false;
            voices[note].live = false;
            endNote(note);
        }
    }
}

void SynthEngine::collectEndedNotes() {
    for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
        ProcessorVoice &v = voices[note];
        if ( v.noteActive && !v.keydown && (!v.live || !v.dx7_note->isPlaying()) )
            endNote(note);
    }
}

void SynthEngine::endNote(int note) {
    voices[note].noteActive = false;
    if ( noteEndSink != nullptr )
        noteEndSink->noteEnded(voices[note].ref);
}

void SynthEngine::panic() {
    for(int i=0;i<MAX_ACTIVE_NOTES;i++) {
        voices[i].midi_note = -1;
        voices[i].keydown = false;
        voices[i].live = false;
        if ( voices[i].dx7_note != NULL ) {
            voices[i].dx7_note->oscSync();
        }
    }
}

void SynthEngine::setEngineType(int c, int tp) {
    Controllers &ctrl = contexts[c].ctrl;
    switch (tp)  {
        case DEXED_ENGINE_MARKI:
            ctrl.core = &engineMkI;
            break;
        case DEXED_ENGINE_OPL:
            ctrl.core = &engineOpl;
            break;
        default:
            ctrl.core = &engineMsfa;
            break;
    }
    contexts[c].engineType = tp;
}

bool SynthEngine::peekVoiceStatus(VoiceStatus &voiceStatus) {
    if ( currentNote == -1 )
        return false;

    // we are trying to find the last "keydown" note
    int note = currentNote;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (voices[note].keydown) {
            voices[note].dx7_note->peekVoiceStatus(voiceStatus);
            return true;
        }
        if ( --note < 0 )
            note = MAX_ACTIVE_NOTES-1;
    }

    // not found; try a live note
    note = currentNote;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        if (voices[note].live) {
            voices[note].dx7_note->peekVoiceStatus(voiceStatus);
            return true;
        }
        if ( --note < 0 )
            note = MAX_ACTIVE_NOTES-1;
    }

    return true;
}

int SynthEngine::tuningTranspositionShift(int c) {
    int transpose = contexts[c].patch[TRANSPOSE];
    if( tuningState->is_standard_tuning() || ! contexts[c].ctrl.transpose12AsScale )
        return transpose - 24;
    else {
        int d144 = transpose;
        if( d144 % 12 == 0 ) {
            int oct = (d144 - 24) / 12;
            int res = oct * tuningState->scale_length();
            return res;
        }
        else
            return transpose - 24;
    }
}

SynthEngine::MidiResult SynthEngine::processMidiMessage(const uint8_t *buf, int channel) {
    uint8_t cmd = buf[0];
    uint8_t cf0 = cmd & 0xf0;

    if( controllers.mpeEnabled && channel != 1 &&
        (
            (cf0 == 0xb0 && buf[1] == 74 ) || //timbre
            (cf0 == 0xd0 ) || // aftertouch
            (cf0 == 0xe0 ) // pb
            )
        ) {
        // OK so find my voice index
        int voiceIndex = -1;
        for( int i=0; i<MAX_ACTIVE_NOTES; ++i ) {
            if( voices[i].keydown && voices[i].channel == channel ) {
                voiceIndex = i;
                break;
            }
        }
        if( voiceIndex >= 0 ) {
            int i = voiceIndex;
            switch(cf0) {
            // THIS IS COMMENTED SINCE mepTimbre and mpePressure is not used
            // case 0xb0:
            //     voices[i].mpeTimbre = (int)buf[2];
            //     voices[i].dx7_note->mpeTimbre = (int)buf[2];
            //     break;
            // case 0xd0:
            //     voices[i].mpePressure = (int)buf[1];
            //     voices[i].dx7_note->mpePressure = (int)buf[1];
            //     break;
            case 0xe0:
                voices[i].mpePitchBend = (int)( buf[1] | (buf[2] << 7) );
                voices[i].dx7_note->mpePitchBend = (int)( buf[1] | ( buf[2] << 7 ) );
                break;
            }
        }
    }
    else {
        switch(cmd & 0xf0) {
        case 0x80 :
            keyup(channel, buf[1], buf[2]);
        return MidiResult::Handled;

        case 0x90 :
            if (!tuningState->is_standard_tuning() || !buf[2] ||
                !MTS_HasMaster(mtsClient) || !MTS_ShouldFilterNote(mtsClient, buf[1], channel - 1))
                keydown(channel, buf[1], buf[2]);
        return MidiResult::Handled;

        case 0xb0 : {
            int ctrl = buf[1];
            int value = buf[2];
                switch(ctrl) {
                // controller values reach every sound (Dexed's controllers are not per channel)
                case 1:
                    for (auto &x : contexts) { x.ctrl.modwheel_cc = value; x.ctrl.refresh(); }
                    break;
                case 2:
                    for (auto &x : contexts) { x.ctrl.breath_cc = value; x.ctrl.refresh(); }
                    break;
                case 4:
                    for (auto &x : contexts) { x.ctrl.foot_cc = value; x.ctrl.refresh(); }
                    break;
                case 5:
                    for (auto &x : contexts) x.ctrl.portamento_cc = value;
                    break;
                case 64:
                    sustain = value > 63;
                    if (!sustain) {
                        for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
                            if (voices[note].sustained && !voices[note].keydown) {
                                voices[note].dx7_note->keyup();
                                voices[note].sustained = false;
                            }
                        }
                    }
                    break;
                case 65:
                    for (auto &x : contexts) x.ctrl.portamento_enable_cc = value >= 64;
                    break;
                case 120:
                    panic();
                    return MidiResult::Panic;
                case 123:
                    for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
                        if (voices[note].keydown)
                            keyup(channel, voices[note].midi_note, 0);
                    }
                    break;
                default:
                    return MidiResult::UnhandledControlChange;
                }
            }
            return MidiResult::Handled;

        case 0xc0 :
            return MidiResult::ProgramChange;

        case 0xd0 :
            for (auto &x : contexts) { x.ctrl.aftertouch_cc = buf[1]; x.ctrl.refresh(); }
            return MidiResult::Handled;

		case 0xe0 :
			for (auto &x : contexts) x.ctrl.pitch_cc = buf[1] | (buf[2] << 7);
            return MidiResult::Handled;
        }
    }
    return MidiResult::Handled;
}
