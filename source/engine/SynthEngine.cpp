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
    engineType = -1;
    monoMode = 0;

    setEngineType(DEXED_ENGINE_MARKI);

    controllers.pitch_range_up = 3;
    controllers.pitch_range_dn = 3;
    controllers.pitch_step = 0;
    controllers.masterTune = 0;

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

void SynthEngine::prepare(double sampleRate) {
    rate.init(sampleRate);
    lfo.init(sampleRate);
    fx.init(sampleRate);

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        voices[note].dx7_note = new Dx7Note(tuningState, mtsClient, &rate);
        voices[note].midi_note = -1;
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
        voices[note].keydown_seq = -1;
    }

    currentNote = 0;
    nextKeydownSeq = 0;
    controllers.pitch_cc = 0x2000;
    controllers.modwheel_cc = 0;
    controllers.foot_cc = 0;
    controllers.breath_cc = 0;
    controllers.aftertouch_cc = 0;
    controllers.portamento_enable_cc = false;
    controllers.portamento_cc = 0;
	controllers.refresh();

    sustain = false;
    extra_buf_size = 0;

    lfo.reset(patch + LFO_PARAMS);
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
    }
}

void SynthEngine::refreshVoices() {
    for(int i=0;i < MAX_ACTIVE_NOTES;i++) {
        if ( voices[i].live )
            voices[i].dx7_note->update(patch, voices[i].midi_note, voices[i].velocity, voices[i].channel);
    }
    lfo.reset(patch + LFO_PARAMS);
}

void SynthEngine::resetLfo() {
    lfo.reset(patch + LFO_PARAMS);
}

void SynthEngine::render(float *channelData, int numSamples, EventSource &events) {
    ScopedFlushDenormals noDenormals;
    int i;

    // flush first events
    for (i=0; i < numSamples && i < extra_buf_size; i++) {
        channelData[i] = extra_buf[i];
    }

    // remaining buffer is still to be processed
    if (extra_buf_size > numSamples) {
        for (int j = 0; j < extra_buf_size - numSamples; j++) {
            extra_buf[j] = extra_buf[j + numSamples];
        }
        extra_buf_size -= numSamples;

        // flush the events, they will be process in the next cycle
        events.dispatchUntil(numSamples);
    } else {
        for (; i < numSamples; i += N) {
            AlignedBuf<int32_t, N> audiobuf;
            float sumbuf[N];

            events.dispatchUntil(i);

            for (int j = 0; j < N; ++j) {
                audiobuf.get()[j] = 0;
                sumbuf[j] = 0;
            }
            int32_t lfovalue = lfo.getsample();
            int32_t lfodelay = lfo.getdelay();

            bool checkMTSESPRetuning = tuningState->is_standard_tuning() &&
                                        MTS_HasMaster(mtsClient);

            for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
                if (voices[note].live) {

                    if (checkMTSESPRetuning)
                        voices[note].dx7_note->updateBasePitches();

                    voices[note].dx7_note->compute(audiobuf.get(), lfovalue, lfodelay, &controllers);

                    for (int j=0; j < N; ++j) {
                        int32_t val = audiobuf.get()[j];

                        val = val >> 4;
                        int clip_val = val < -(1 << 24) ? 0x8000 : val >= (1 << 24) ? 0x7fff : val >> 9;
                        float f = ((float) clip_val) / (float) 0x8000;
                        if( f > 1 ) f = 1;
                        if( f < -1 ) f = -1;
                        sumbuf[j] += f;
                        audiobuf.get()[j] = 0;
                    }
                }
            }

            int jmax = numSamples - i;
            for (int j = 0; j < N; ++j) {
                if (j < jmax) {
                    channelData[i + j] = sumbuf[j];
                } else {
                    extra_buf[j - jmax] = sumbuf[j];
                }
            }
        }
        extra_buf_size = i - numSamples;
    }

    events.dispatchUntil(numSamples);
    fx.process(channelData, numSamples);
}

void SynthEngine::keydown(uint8_t channel, uint8_t pitch, uint8_t velo) {
    if ( velo == 0 ) {
        keyup(channel, pitch, velo);
        return;
    }

    pitch += tuningTranspositionShift();

    if ( normalizeDxVelocity ) {
        velo = ((float)velo) * 0.7874015; // 100/127
    }

    // Single fused pass: voice-stealing scoring + triggerLfo (any keydown?)
    // + MPE same-channel-keydown detection.
    // Stealing preference (highest score wins; ties broken by least-recent keydown):
    //   +4 not playing, +2 key up, +1 same pitch.
    bool triggerLfo = true;
    int bestNote   = currentNote;
    int bestScore  = -1;
    int note       = currentNote;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        const ProcessorVoice &v = voices[note];

        if ( v.keydown ) {
            triggerLfo = false;
            // Two keydowns on the same channel => source is not MPE.
            if ( controllers.mpeEnabled && v.channel == channel )
                controllers.mpeEnabled = false;
        }

        int score = 0;
        if ( !v.dx7_note->isPlaying() ) score += 4;
        if ( !v.keydown )               score += 2;
        if (  v.midi_note == pitch )    score += 1;
        if ( score > bestScore ||
             (score == bestScore && v.keydown_seq < voices[bestNote].keydown_seq) ) {
            bestNote  = note;
            bestScore = score;
        }

        note = (note + 1) % MAX_ACTIVE_NOTES;
    }

    if ( triggerLfo )
        lfo.keydown();

    note = bestNote;

    currentNote = (note + 1) % MAX_ACTIVE_NOTES;
    voices[note].channel = channel;
    voices[note].midi_note = pitch;
    voices[note].velocity = velo;
    voices[note].sustained = sustain;
    voices[note].keydown = true;
    voices[note].keydown_seq = nextKeydownSeq++;
    // to avoid click, don't sync oscillators when voice stealing
    bool voice_steal = voices[note].dx7_note->isPlaying();
    voices[note].dx7_note->init(patch, pitch, velo, channel, &controllers);
    if ( patch[OSC_KEY_SYNC] && !voice_steal ) {
        voices[note].dx7_note->oscSync();
    }
    if ( (voices[lastActiveVoice].midi_note != -1 && controllers.portamento_enable_cc)
       && controllers.portamento_cc > 0 ) {
        voices[note].dx7_note->initPortamento(*voices[lastActiveVoice].dx7_note);
    }

    if ( monoMode ) {
        for(int i=0; i<MAX_ACTIVE_NOTES; i++) {
            if ( voices[i].live ) {
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
            if ( i != note && voices[i].dx7_note->isPlaying() && voices[i].midi_note == pitch ) {
                voices[note].dx7_note->transferPhase(*voices[i].dx7_note);
                break;
            }
        }
    }

    voices[note].live = true;
    lastActiveVoice = note;
}

void SynthEngine::keyup(uint8_t chan, uint8_t pitch, uint8_t velo) {
    pitch += tuningTranspositionShift();

    int note;
    for (note=0; note<MAX_ACTIVE_NOTES; ++note) {
        if ( ( ( controllers.mpeEnabled && voices[note].channel == chan ) || // MPE node - find voice by channel
               (!controllers.mpeEnabled && voices[note].midi_note == pitch ) ) && // regular mode find voice by pitch
            voices[note].keydown ) { // but still only grab the one which is keydown
            voices[note].keydown = false;
            break;
        }
    }

    // note not found ?
    if ( note >= MAX_ACTIVE_NOTES ) {
        return;
    }

    if ( monoMode ) {
        int highNote = -1;
        int target = 0;
        for (int i=0; i<MAX_ACTIVE_NOTES;i++) {
            if ( voices[i].keydown && voices[i].midi_note > highNote ) {
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

int SynthEngine::getEngineType() {
    return engineType;
}

void SynthEngine::setEngineType(int tp) {
    switch (tp)  {
        case DEXED_ENGINE_MARKI:
            controllers.core = &engineMkI;
            break;
        case DEXED_ENGINE_OPL:
            controllers.core = &engineOpl;
            break;
        default:
            controllers.core = &engineMsfa;
            break;
    }
    engineType = tp;
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

int SynthEngine::tuningTranspositionShift() {
    int transpose = patch[TRANSPOSE];
    if( tuningState->is_standard_tuning() || ! controllers.transpose12AsScale )
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
                case 1:
                    controllers.modwheel_cc = value;
                    controllers.refresh();
                    break;
                case 2:
                    controllers.breath_cc = value;
                    controllers.refresh();
                    break;
                case 4:
                    controllers.foot_cc = value;
                    controllers.refresh();
                    break;
                case 5:
                    controllers.portamento_cc = value;
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
                    controllers.portamento_enable_cc = value >= 64;
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
            controllers.aftertouch_cc = buf[1];
            controllers.refresh();
            return MidiResult::Handled;

		case 0xe0 :
			controllers.pitch_cc = buf[1] | (buf[2] << 7);
            return MidiResult::Handled;
        }
    }
    return MidiResult::Handled;
}
