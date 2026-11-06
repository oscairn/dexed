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

#include <algorithm>
#include "msfa/ext_tables.h"
#include "msfa/ext_waves.h"

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
        (void)dx7Compiled(0); // the compiled DX7 algorithms
        extwave::init();      // the Extended profile's operator waves
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
    resetControllers();

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
    this->sampleRate = sampleRate;
    rate.init(sampleRate);
    for (int c = 1; c < MAX_CONTEXTS; c++) {
        contexts[c].blockRight.assign((size_t)(maxFrames > 0 ? maxFrames : 0), 0.0f);
        contexts[c].block.assign((size_t)(maxFrames > 0 ? maxFrames : 0), 0.0f);
    }

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        delete voices[note].dx7_note; // prepared again without a release: rebuild for the new rate
        voices[note].dx7_note = new Dx7Note(tuningState, mtsClient, &rate);
        voices[note].noteActive = false;
        voices[note].context = 0;
    }
    restart();
}

// As a fresh engine after prepare, without the allocations: voices as new, the LFOs at phase 0
// (prepare leaves a free-running LFO's phase alone, as Dexed does), filters, buffers and
// controllers cleared; the parameters stay as set. A note that was playing is reported ended
// at the next render.
void SynthEngine::reset() {
    for (int c = 0; c < MAX_CONTEXTS; c++)
        contexts[c].lfo = Lfo();
    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note)
        if ( voices[note].dx7_note != NULL )
            voices[note].dx7_note->reset();
    restart();
}

// The part of prepare that allocates nothing: no voice sounding, the LFOs, filters, buffers
// and controllers as prepare leaves them.
void SynthEngine::restart() {
    for (int c = 0; c < MAX_CONTEXTS; c++) {
        contexts[c].lfo.init(sampleRate);
        contexts[c].partLfo = ExtPartLfo();
        contexts[c].partLfo.init(sampleRate);
        contexts[c].fx.init(sampleRate);
        contexts[c].noteRandom = 0x2545F491u + (uint32_t)c;
        contexts[c].noteCount = 0;
        contexts[c].fxRight.init(sampleRate);
        contexts[c].rendering = false;
        contexts[c].tail = false;
        contexts[c].chokeLeft = contexts[c].chokeTotal = 0;
        contexts[c].choked = false;
    }

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        voices[note].midi_note = -1;
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
        voices[note].keydown_seq = -1;
        voices[note].fadeLeft = voices[note].fadeTotal = 0;
        if ( voices[note].dx7_note != NULL )
            voices[note].dx7_note->oscSync();
    }

    currentNote = 0;
    glideClock = 0;
    glideTick = false;
    nextKeydownSeq = 0;
    resetControllers();

    for (int ch = 0; ch < 16; ch++)
        sustain[ch] = false;
    extra_buf_size = 0;

    for (int c = 0; c < MAX_CONTEXTS; c++)
        if ( contexts[c].patch != nullptr )
            resetLfo(c);
}

// The sounds' and channels' controller values as a new engine has them (Controllers' constructor
// leaves them unset): from the constructor, so that parameter changes before the first prepare copy
// set values, and from prepare.
void SynthEngine::resetControllers() {
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
        for (int ch = 0; ch < 16; ch++)
            channelCtrl[c][ch] = ctrl;
    }
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
            voices[i].dx7_note->update(x.patch, voices[i].midi_note, voices[i].velocity, voices[i].channel, x.ext);
    }
    resetLfo(c);
}

void SynthEngine::resetLfo(int c) {
    SynthContext &x = contexts[c];
    if ( x.extended() ) {
        x.lfo.reset(*x.ext);
        x.partLfo.configure(*x.ext, tempo);
    } else
        x.lfo.reset(x.patch + LFO_PARAMS);
}

void SynthEngine::panicContext(int c) {
    contexts[c].chokeLeft = contexts[c].chokeTotal = 0;
    contexts[c].choked = false;
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

unsigned SynthEngine::controllerTargets(int ch) {
    if ( !perChannel || (contexts[0].ctrl.mpeEnabled && ch == 1) )
        return 0xffff;
    return 1u << ((ch - 1) & 15);
}

void SynthEngine::syncChannelControllers(int c) {
    for (int ch = 0; ch < 16; ch++) {
        Controllers &d = channelCtrl[c][ch];
        Controllers keep = d;
        d = contexts[c].ctrl;
        d.pitch_cc = keep.pitch_cc;
        d.modwheel_cc = keep.modwheel_cc;
        d.breath_cc = keep.breath_cc;
        d.foot_cc = keep.foot_cc;
        d.aftertouch_cc = keep.aftertouch_cc;
        d.portamento_cc = keep.portamento_cc;
        d.portamento_enable_cc = keep.portamento_enable_cc;
        d.mpeEnabled = keep.mpeEnabled;
        d.refresh();
    }
}

void SynthEngine::setPortamento(int c, int32_t time, bool enable) {
    contexts[c].ctrl.portamento_cc = time;
    contexts[c].ctrl.portamento_enable_cc = enable;
    for (int ch = 0; ch < 16; ch++) {
        channelCtrl[c][ch].portamento_cc = time;
        channelCtrl[c][ch].portamento_enable_cc = enable;
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

bool SynthEngine::silent() const {
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
        if ( voices[i].live || voices[i].noteActive )
            return false;
    for (const SynthContext &x : contexts)
        if ( x.tail )
            return false;
    return true;
}

namespace {
    // The events of a longer block, seen from a part of it that starts at offset.
    struct OffsetEvents : SynthEngine::EventSource {
        SynthEngine::EventSource &inner;
        int offset;
        OffsetEvents(SynthEngine::EventSource &inner, int offset) : inner(inner), offset(offset) {}
        void dispatchUntil(int samplePos) override { inner.dispatchUntil(samplePos + offset); }
    };
}

void SynthEngine::render(float *channelData, float *right, int numSamples, EventSource &events, float *const *ring) {
    // A block longer than the contexts' buffers (prepare's maxFrames): render it in parts,
    // so every sound keeps playing. Output does not depend on block size.
    const int maxBlock = (int)contexts[1].block.size();
    if ( maxBlock > 0 && numSamples > maxBlock ) {
        for (int offset = 0; offset < numSamples; offset += maxBlock) {
            const int n = numSamples - offset < maxBlock ? numSamples - offset : maxBlock;
            OffsetEvents part(events, offset);
            float *ringPart[4] = { nullptr, nullptr, nullptr, nullptr };
            for (int k = 0; ring != nullptr && k < 4; k++)
                ringPart[k] = ring[k] + offset;
            render(channelData + offset, right != nullptr ? right + offset : nullptr, n, part, ring != nullptr ? ringPart : nullptr);
        }
        return;
    }
    ScopedFlushDenormals noDenormals;
    int i;

    // Contexts that play this block: the default one (as the single-sound engine always
    // did), and any other that is selected, has voices or has not yet gone silent.
    bool active[MAX_CONTEXTS] = { true };
    float *outputs[MAX_CONTEXTS] = { channelData };
    float *outputsRight[MAX_CONTEXTS] = { right };
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
                x.extra_buf[j] = x.extra_bufRight[j] = 0;
        x.rendering = true;
        active[c] = true;
        outputs[c] = x.block.data();
        outputsRight[c] = x.blockRight.data();
    }

    // flush first events
    for (int c = 0; c < MAX_CONTEXTS; c++) {
        if ( !active[c] )
            continue;
        for (i=0; i < numSamples && i < extra_buf_size; i++) {
            outputs[c][i] = contexts[c].extra_buf[i];
            if ( right != nullptr )
                outputsRight[c][i] = contexts[c].extra_bufRight[i];
        }
    }
    i = numSamples < extra_buf_size ? numSamples : extra_buf_size;

    // remaining buffer is still to be processed
    if (extra_buf_size > numSamples) {
        for (int c = 0; c < MAX_CONTEXTS; c++) {
            if ( !active[c] )
                continue;
            float *extra_buf = contexts[c].extra_buf;
            float *extra_bufRight = contexts[c].extra_bufRight;
            for (int j = 0; j < extra_buf_size - numSamples; j++) {
                extra_buf[j] = extra_buf[j + numSamples];
                extra_bufRight[j] = extra_bufRight[j + numSamples];
            }
        }
        extra_buf_size -= numSamples;

        // flush the events, they will be process in the next cycle
        events.dispatchUntil(numSamples);
    } else {
        for (; i < numSamples; i += N) {
            AlignedBuf<int32_t, N> audiobuf;
            float sumbuf[MAX_CONTEXTS][N];
            float sumbufRight[MAX_CONTEXTS][N]; // (stereo rendering)

            events.dispatchUntil(i);

            // Extended portamento: one clock for every sound, running freely (ExtNote::kGlideTickSeconds)
            // (counted in samples at 44.1 kHz, exact there)
            glideClock += N * 44100.0 / sampleRate;
            glideTick = glideClock >= ExtNote::kGlideTickSeconds * 44100.0;
            if ( glideTick )
                glideClock -= ExtNote::kGlideTickSeconds * 44100.0;

            for (int j = 0; j < N; ++j) {
                audiobuf.get()[j] = 0;
            }
            int32_t lfovalue[MAX_CONTEXTS], lfodelay[MAX_CONTEXTS];
            float partlfo[MAX_CONTEXTS][N];      // Extended sounds: the part LFO per sample
            bool partActive[MAX_CONTEXTS] = {};
            int lfoSteps[MAX_CONTEXTS] = {};     // Extended sounds: the part LFO's LFO 2 speed move
            for (int c = 0; c < MAX_CONTEXTS; c++) {
                if ( !active[c] )
                    continue;
                for (int j = 0; j < N; ++j)
                    sumbuf[c][j] = sumbufRight[c][j] = 0;
                if ( contexts[c].extended() && contexts[c].partLfo.active() ) {
                    contexts[c].partLfo.render(partlfo[c], N);
                    partActive[c] = true;
                    // the LFO 2 speed destination: depth x 100 / 128 speed steps at the part
                    // LFO's top, rounded towards 0, once per block
                    lfoSteps[c] = contexts[c].partLfo.lfo2Steps(partlfo[c][N / 2]);
                    contexts[c].lfo.setSpeedSteps(lfoSteps[c]);
                }
                if ( contexts[c].lfo.perSample() ) {
                    // Extended voices run their own copies; the sound's runs on for the phase a
                    // note without key-on reset takes
                    for (int j = 0; j < N; ++j)
                        lfovalue[c] = contexts[c].lfo.getsample();
                } else {
                    lfovalue[c] = contexts[c].lfo.getsample();
                }
                lfodelay[c] = contexts[c].lfo.getdelay();
            }

            bool checkMTSESPRetuning = tuningState->is_standard_tuning() &&
                                        MTS_HasMaster(mtsClient);

            for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
                const int c = voices[note].context;
                if (voices[note].live && active[c]) {

                    if (checkMTSESPRetuning)
                        voices[note].dx7_note->updateBasePitches();

                    voices[note].dx7_note->compute(audiobuf.get(), lfovalue[c], lfodelay[c], &voiceCtrl(c, voices[note].channel),
                                                   partActive[c] ? partlfo[c] : nullptr, lfoSteps[c]);

                    ProcessorVoice &v = voices[note];
                    float gainLeft = 1, gainRight = 1;
                    const bool panned = right != nullptr && v.dx7_note->panned(gainLeft, gainRight);
                    for (int j=0; j < N; ++j) {
                        int32_t val = audiobuf.get()[j];
                        float f;
                        if (v.dx7_note->extended()) {
                            // Extended voices skip Dexed's 16-bit rounding: on a periodic tone its
                            // error lands on the harmonics, about 70 dB down, well above the
                            // reference's own floor there; and its clipping at full scale (the
                            // loudest algorithms pass it at the reference's own level, which does
                            // not clip)
                            f = (float)val * (kExtendedVoiceGain / (float)(1 << 28));
                        } else {
                            val = val >> 4;
                            int clip_val = val < -(1 << 24) ? 0x8000 : val >= (1 << 24) ? 0x7fff : val >> 9;
                            f = ((float) clip_val) / (float) 0x8000;
                            if( f > 1 ) f = 1;
                            if( f < -1 ) f = -1;
                        }
                        if ( v.fadeTotal > 0 ) { // choked: fading out
                            const float g = (float)v.fadeLeft / (float)v.fadeTotal;
                            f *= v.fadeSquared ? g * g : g;
                            if ( v.fadeLeft > 0 )
                                v.fadeLeft--;
                        }
                        if ( panned ) {
                            sumbuf[c][j] += f * gainLeft;
                            sumbufRight[c][j] += f * gainRight;
                        } else {
                            sumbuf[c][j] += f;
                            sumbufRight[c][j] += f;
                        }
                        audiobuf.get()[j] = 0;
                    }
                    if ( v.fadeTotal > 0 && v.fadeLeft == 0 ) {
                        // the fade is over: free the voice
                        v.fadeTotal = 0;
                        v.live = false;
                        v.dx7_note->silence();
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
                if ( right != nullptr ) {
                    float *outRight = outputsRight[c];
                    for (int j = 0; j < N; ++j) {
                        if (j < jmax)
                            outRight[i + j] = sumbufRight[c][j];
                        else
                            contexts[c].extra_bufRight[j - jmax] = sumbufRight[c][j];
                    }
                }
            }
        }
        extra_buf_size = i - numSamples;
    }

    events.dispatchUntil(numSamples);
    // Each context through its own output filter, then summed (RFC 0005): context 0 in place,
    // the others added to it, a retiring one into the ring-out buffer instead.
    for (int c = 0; c < MAX_CONTEXTS; c++) {
        if ( !active[c] )
            continue;
        SynthContext &x = contexts[c];
        float *out = outputs[c], *outRight = outputsRight[c];
        x.fx.setExtendedDc(x.extended());
        x.fxRight.setExtendedDc(x.extended());
        x.fx.process(out, numSamples);
        if ( right != nullptr )
            processRight(x, outRight, numSamples);
        if ( x.selectedChannels != 0 ) { // selected again: plays on in full
            x.chokeLeft = x.chokeTotal = 0;
            x.choked = false;
        }
        if ( x.chokeTotal > 0 ) { // choked: fading out (its filter's tail with its voices), then silent
            for (int j = 0; j < numSamples; j++) {
                const float g = (float)x.chokeLeft / (float)x.chokeTotal;
                out[j] *= g;
                if ( right != nullptr )
                    outRight[j] *= g;
                if ( x.chokeLeft > 0 )
                    x.chokeLeft--;
            }
        }
        // the context still sounds (its filter rings on), here or in what is rendered ahead; below
        // a 24-bit step is silence, as the output filter can settle on a tiny value instead of 0
        const float silence = 1.0f / (1 << 24);
        bool sound = false;
        for (int j = 0; j < numSamples && !sound; j++)
            sound = std::fabs(out[j]) >= silence || (right != nullptr && std::fabs(outRight[j]) >= silence);
        for (int j = 0; j < extra_buf_size && !sound; j++)
            sound = std::fabs(x.extra_buf[j]) >= silence || (right != nullptr && std::fabs(x.extra_bufRight[j]) >= silence);
        x.tail = sound;
        const bool retiring = ring != nullptr && x.selectedChannels == 0;
        if ( c == 0 ) {
            if ( retiring ) {
                float *toLeft = ring[x.choked ? 2 : 0], *toRight = ring[x.choked ? 3 : 1];
                for (int j = 0; j < numSamples; j++) {
                    toLeft[j] += channelData[j];
                    toRight[j] += right != nullptr ? right[j] : channelData[j];
                    channelData[j] = 0;
                    if ( right != nullptr )
                        right[j] = 0;
                }
            }
            continue;
        }
        float *toLeft = retiring ? ring[x.choked ? 2 : 0] : channelData, *toRight = retiring ? ring[x.choked ? 3 : 1] : right;
        for (int j = 0; j < numSamples; j++)
            toLeft[j] += out[j];
        if ( right == nullptr && retiring )
            for (int j = 0; j < numSamples; j++)
                toRight[j] += out[j];
        if ( right != nullptr )
            for (int j = 0; j < numSamples; j++)
                toRight[j] += outRight[j];
    }
}

// The right channel through its own output filter, with the left one's settings: given the
// same input it gives the same output, so unpanned sounds stay identical in both channels.
void SynthEngine::processRight(SynthContext &x, float *right, int numSamples) {
    x.fxRight.uiCutoff = x.fx.uiCutoff;
    x.fxRight.uiReso = x.fx.uiReso;
    x.fxRight.uiGain = x.fx.uiGain;
    x.fxRight.process(right, numSamples);
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

    const int beyond = keyBeyond(c, pitch);
    pitch = shiftedKey(c, pitch);

    if ( normalizeDxVelocity ) {
        velo = ((float)velo) * 0.7874015; // 100/127
    }
    double extVelocity = 0;
    if ( x.extended() ) {
        // the part's velocity sensitivity; at 0 or below the note does not sound
        const double v = extPartVelocity(x.ext->common(ext::VelocityDepth), x.ext->common(ext::VelocityOffset), velo);
        if ( v <= 0 ) {
            if ( noteEndSink != nullptr )
                noteEndSink->noteEnded(ref);
            return;
        }
        velo = (uint8_t)std::clamp((int)std::lround(v), 1, 127);
        extVelocity = v;
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
                for (int k = 0; k < MAX_CONTEXTS; k++) {
                    contexts[k].ctrl.mpeEnabled = false;
                    for (int ch = 0; ch < 16; ch++)
                        channelCtrl[k][ch].mpeEnabled = false;
                }
        }
    }

    if ( triggerLfo )
        x.lfo.keydown();
    if ( x.extended() )
        x.partLfo.keydown(triggerLfo);

    if ( voiceGiver != nullptr )
        voiceGiver->makeRoom(c);
    int note = VoiceStealing::choose(voices, MAX_ACTIVE_NOTES, currentNote, pitch);
    if ( voiceGiver != nullptr && voices[note].dx7_note->isPlaying() ) { // no free voice
        const int given = voiceGiver->steal(c);
        if ( given >= 0 )
            note = given;
    }

    if ( voices[note].noteActive )
        endNote(note); // stolen
    voices[note].ref = ref;
    voices[note].noteActive = true;
    voices[note].context = c;

    currentNote = (note + 1) % MAX_ACTIVE_NOTES;
    voices[note].channel = channel;
    voices[note].midi_note = pitch;
    voices[note].keyBeyond = beyond;
    voices[note].velocity = velo;
    voices[note].sustained = sustain[(channel - 1) & 15];
    Controllers &ctrl = voiceCtrl(c, channel);
    voices[note].keydown = true;
    voices[note].fadeLeft = voices[note].fadeTotal = 0; // a voice taken while fading
    voices[note].keydown_seq = nextKeydownSeq++;
    if ( x.extended() ) {
        voices[note].velocityExact = extVelocity;
        extKeydown(note, c);
        return;
    }
    // to avoid click, don't sync oscillators when voice stealing
    bool voice_steal = voices[note].dx7_note->isPlaying();
    ExtStart start;
    voices[note].dx7_note->init(patch, pitch, velo, channel, &ctrl, x.ext, start);
    if ( patch[OSC_KEY_SYNC] && !voice_steal ) {
        voices[note].dx7_note->oscSync();
    }
    // (the last voice of this sound: another sound may have taken it since)
    if ( (voices[x.lastActiveVoice].midi_note != -1 && voices[x.lastActiveVoice].context == c &&
          ctrl.portamento_enable_cc) && ctrl.portamento_cc > 0 ) {
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

// ---- Extended sounds: mono, key assign and portamento (measured, docs/oscairn-plan.md M7) ----

ExtStart SynthEngine::nextExtStart(SynthContext &x, int64_t wait) {
    // deterministic: the same notes after prepare give the same values (xorshift32)
    auto next = [&x] {
        uint32_t &r = x.noteRandom;
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return r;
    };
    ExtStart start;
    start.randomPitch = next();
    start.randomPan = next();
    start.count = x.noteCount++;
    start.tempo = tempo;
    start.wait = wait;
    start.lfoPhase = x.lfo.phase();
    return start;
}

void SynthEngine::startVoice(int note, int c, int64_t wait) {
    ProcessorVoice &v = voices[note];
    SynthContext &x = contexts[c];
    // to avoid a click, a stolen voice keeps its phases (Extended sounds reset each operator by
    // its own Key On Reset)
    const bool steal = v.dx7_note->isPlaying();
    ExtStart start = nextExtStart(x, wait);
    start.velocity = v.velocityExact;
    start.keyBeyond = v.keyBeyond;
    v.dx7_note->init(x.patch, v.midi_note, v.velocity, v.channel, &voiceCtrl(c, v.channel), x.ext, start);
    if ( !steal )
        v.dx7_note->oscSync();
    v.fadeLeft = v.fadeTotal = 0;
}

void SynthEngine::fadeVoice(int note, double seconds, bool squared) {
    ProcessorVoice &v = voices[note];
    if ( v.live && v.fadeTotal == 0 ) {
        v.fadeTotal = v.fadeLeft = (int)(seconds * sampleRate) + 1;
        v.fadeSquared = squared;
    }
}

int SynthEngine::liveVoice(int c, int except) const {
    int best = -1;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
        if ( i != except && voices[i].live && voices[i].context == c && voices[i].fadeTotal == 0 &&
             (best < 0 || voices[i].keydown_seq > voices[best].keydown_seq) )
            best = i;
    return best;
}

void SynthEngine::extGlide(int note, int c, int from, bool legato) {
    const ExtendedVoice &e = *contexts[c].ext;
    if ( !e.common(ext::Portamento) || from < 0 || from == note || voices[from].context != c ||
         voices[from].midi_note < 0 || !voices[from].dx7_note->extended() )
        return;
    if ( !legato && e.common(ext::PortamentoMode) == 0 )
        return; // fingered: legato notes only
    const double octave = 1 << 24, b = voices[note].dx7_note->extPitch();
    double a = voices[from].dx7_note->extPitch();
    if ( e.common(ext::PortamentoTimeMode) >= 2 ) // rate 2, time 2: from at most an octave away
        a = std::clamp(a, b - octave, b + octave);
    const double semitones = std::fabs(a - b) * 12.0 / octave;
    voices[note].dx7_note->glideFrom(a, extPortamentoRate(e.common(ext::PortamentoTime),
                                                          e.common(ext::PortamentoTimeMode), semitones),
                                         &glideTick);
}

void SynthEngine::extKeydown(int note, int c) {
    SynthContext &x = contexts[c];
    const ExtendedVoice &e = *x.ext;
    const bool mono = e.common(ext::MonoPoly) == 0, porta = e.common(ext::Portamento) != 0;
    bool held = false; // another key of this sound is down: a legato note
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
        held |= i != note && voices[i].keydown && voices[i].context == c;
    const int prev = liveVoice(c, note);
    int retrigger = -1;
    if ( mono && prev >= 0 && !(porta && held) ) {
        retrigger = prev; // mono without a portamento legato: the note starts again
    } else if ( !mono && e.common(ext::KeyAssign) == 0 ) {
        // single: a key played again silences its sounding note
        for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
            if ( i != note && voices[i].live && voices[i].context == c && voices[i].fadeTotal == 0 &&
                 voices[i].midi_note == voices[note].midi_note && voices[i].keyBeyond == voices[note].keyBeyond )
                retrigger = i;
    }
    if ( retrigger >= 0 ) // a note that takes over without portamento: the old one fades out as it starts
        fadeVoice(retrigger, ExtNote::kRetriggerFade, true);
    // a key pressed again while held: one note-off releases nothing until every press is released
    voices[note].presses = 1;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
        if ( i != note && voices[i].keydown && voices[i].context == c && voices[i].channel == voices[note].channel &&
             voices[i].midi_note == voices[note].midi_note && voices[i].keyBeyond == voices[note].keyBeyond ) {
            voices[note].presses += voices[i].presses;
            voices[i].presses = 0;
        }
    startVoice(note, c, 0);
    if ( mono && prev >= 0 && retrigger < 0 ) {
        // a mono legato note (portamento on): the envelopes carry on
        voices[note].dx7_note->transferState(*voices[prev].dx7_note);
        voices[prev].live = false;
    }
    if ( porta )
        extGlide(note, c, x.lastActiveVoice, held);
    voices[note].live = true;
    x.lastActiveVoice = note;
}

// A mono sound's sounding key released while others are held: the most recent of them sounds
// again (legato with portamento, otherwise starting again). True when it took over.
bool SynthEngine::extKeyup(int note) {
    const int c = voices[note].context;
    SynthContext &x = contexts[c];
    if ( x.ext->common(ext::MonoPoly) != 0 || !voices[note].live || voices[note].fadeTotal != 0 )
        return false;
    int h = -1;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
        if ( i != note && voices[i].keydown && voices[i].context == c &&
             (h < 0 || voices[i].keydown_seq > voices[h].keydown_seq) )
            h = i;
    if ( h < 0 )
        return false;
    const bool porta = x.ext->common(ext::Portamento) != 0;
    if ( !porta )
        fadeVoice(note, ExtNote::kRetriggerFade, true);
    startVoice(h, c, 0);
    if ( porta ) {
        voices[h].dx7_note->transferState(*voices[note].dx7_note);
        extGlide(h, c, note, true);
        voices[note].live = false;
    }
    voices[h].live = true;
    x.lastActiveVoice = h;
    return true;
}

// A MIDI note-off of an Extended sound: a key pressed n times is released by its n-th note-off,
// then every voice it holds (measured with the same key twice, single and multi).
void SynthEngine::extRelease(int note) {
    const int c = voices[note].context, ch = voices[note].channel, key = voices[note].midi_note,
              beyond = voices[note].keyBeyond;
    auto same = [&](int i) {
        return voices[i].keydown && voices[i].context == c && voices[i].channel == ch && voices[i].midi_note == key &&
               voices[i].keyBeyond == beyond;
    };
    int lead = note;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
        if ( same(i) && voices[i].presses > voices[lead].presses )
            lead = i;
    if ( --voices[lead].presses > 0 )
        return;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++)
        if ( same(i) ) {
            voices[i].keydown = false;
            keyupVoice(i);
        }
}

int SynthEngine::findKeyupVoice(uint8_t chan, uint8_t pitch, unsigned channels) {
    for (int note=0; note<MAX_ACTIVE_NOTES; ++note) {
        // the pitch as the voice's sound transposed it
        // a held voice on the same channel and key (with MPE too: a member channel's
        // note-off carries its note's key); Dexed matched only the channel with MPE on, which
        // is the default, and only the key without it
        if ( voices[note].keydown &&
             (channels & (1u << ((voices[note].channel - 1) & 15))) &&
             voices[note].channel == chan &&
             voices[note].midi_note == shiftedKey(voices[note].context, pitch) &&
             voices[note].keyBeyond == keyBeyond(voices[note].context, pitch) ) {
            return note;
        }
    }
    return -1;
}

void SynthEngine::keyup(uint8_t chan, uint8_t pitch, uint8_t velo) {
    const int note = findKeyupVoice(chan, pitch, 0xffff);

    // note not found ?
    if ( note < 0 ) {
        return;
    }
    if ( contexts[voices[note].context].extended() ) {
        extRelease(note);
        return;
    }

    voices[note].keydown = false;
    keyupVoice(note);
}

void SynthEngine::keyupVoice(int note) {
    const int c = voices[note].context;
    if ( contexts[c].extended() ) {
        if ( extKeyup(note) )
            return; // the sound's next held key took over
    } else if ( contexts[c].monoMode ) {
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

    if ( sustain[(voices[note].channel - 1) & 15] ) {
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
    for (int note = 0; note < MAX_ACTIVE_NOTES; note++)
        if ( voices[note].noteActive && matches(voices[note].ref, match) )
            chokeVoice(note);
}

void SynthEngine::chokeVoice(int note) {
    ProcessorVoice &v = voices[note];
    v.keydown = false;
    v.sustained = false;
    if ( v.live && v.fadeLeft == 0 ) {
        // a short fade-out instead of a click; the voice is freed (and its NOTE_END
        // sent by collectEndedNotes) when it ends
        v.fadeTotal = v.fadeLeft = (int)(0.003 * sampleRate) + 1;
        v.fadeSquared = false;
    } else if ( !v.live ) {
        endNote(note);
    }
}

void SynthEngine::chokeRetiring() {
    bool sounds[MAX_CONTEXTS] = {};
    for (int note = 0; note < MAX_ACTIVE_NOTES; note++)
        if ( voices[note].live || voices[note].noteActive )
            sounds[voices[note].context] = true;
    for (int c = 0; c < MAX_CONTEXTS; c++) {
        SynthContext &x = contexts[c];
        if ( x.selectedChannels != 0 || !(sounds[c] || x.tail) || x.choked )
            continue;
        x.choked = true;
        x.chokeTotal = x.chokeLeft = (int)(0.003 * sampleRate) + 1;
        for (int note = 0; note < MAX_ACTIVE_NOTES; note++)
            if ( voices[note].context == c && (voices[note].live || voices[note].noteActive) )
                chokeVoice(note);
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

// The key a voice of context c plays for a played key: Classic as Dexed (wrapping), Extended
// with the part's note shift too, held within 0-127; keyBeyond is how far an Extended key's shift
// went past that range (in semitones, played as pitch).
uint8_t SynthEngine::shiftedKey(int c, int key) {
    if ( contexts[c].extended() )
        return (uint8_t)std::clamp(key + tuningTranspositionShift(c) + std::clamp(contexts[c].ext->common(ext::NoteShift), -127, 127), 0, 127);
    return (uint8_t)(key + tuningTranspositionShift(c));
}

int SynthEngine::keyBeyond(int c, int key) {
    if ( !contexts[c].extended() )
        return 0;
    const int k = key + tuningTranspositionShift(c) + std::clamp(contexts[c].ext->common(ext::NoteShift), -127, 127);
    return k - std::clamp(k, 0, 127);
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
                // instrument-wide mode: controller values reach every sound and voice
                // (Dexed); per-channel mode: only the voices of the targeted channels
                case 1: case 2: case 4: {
                    auto set = [&](Controllers &x) {
                        (ctrl == 1 ? x.modwheel_cc : ctrl == 2 ? x.breath_cc : x.foot_cc) = value;
                        x.refresh();
                    };
                    if ( !perChannel ) {
                        for (auto &x : contexts) set(x.ctrl);
                    } else {
                        const unsigned t = controllerTargets(channel);
                        for (int c = 0; c < MAX_CONTEXTS; c++)
                            for (int ch = 0; ch < 16; ch++)
                                if ( t & (1u << ch) ) set(channelCtrl[c][ch]);
                    }
                    break;
                }
                case 5:
                    if ( !perChannel ) {
                        for (auto &x : contexts) x.ctrl.portamento_cc = value;
                    } else {
                        const unsigned t = controllerTargets(channel);
                        for (int c = 0; c < MAX_CONTEXTS; c++)
                            for (int ch = 0; ch < 16; ch++)
                                if ( t & (1u << ch) ) channelCtrl[c][ch].portamento_cc = value;
                    }
                    break;
                case 64: {
                    const unsigned t = controllerTargets(channel);
                    for (int ch = 0; ch < 16; ch++)
                        if ( t & (1u << ch) ) sustain[ch] = value > 63;
                    if ( !(value > 63) ) {
                        for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
                            if (voices[note].sustained && !voices[note].keydown &&
                                (t & (1u << ((voices[note].channel - 1) & 15)))) {
                                voices[note].dx7_note->keyup();
                                voices[note].sustained = false;
                            }
                        }
                    }
                    break;
                }
                case 65:
                    if ( !perChannel ) {
                        for (auto &x : contexts) x.ctrl.portamento_enable_cc = value >= 64;
                    } else {
                        const unsigned t = controllerTargets(channel);
                        for (int c = 0; c < MAX_CONTEXTS; c++)
                            for (int ch = 0; ch < 16; ch++)
                                if ( t & (1u << ch) ) channelCtrl[c][ch].portamento_enable_cc = value >= 64;
                    }
                    break;
                case 120:
                    if ( !perChannel ) {
                        panic();
                        return MidiResult::Panic;
                    } else {
                        // all sound off for the channel: its voices stop now and end
                        const unsigned t = controllerTargets(channel);
                        for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
                            if ( !(t & (1u << ((voices[i].channel - 1) & 15))) )
                                continue;
                            voices[i].midi_note = -1;
                            voices[i].keydown = false;
                            voices[i].sustained = false;
                            voices[i].live = false;
                            if ( voices[i].dx7_note != NULL )
                                voices[i].dx7_note->oscSync();
                            if ( voices[i].noteActive )
                                endNote(i);
                        }
                        return MidiResult::Handled;
                    }
                case 123: {
                    // all notes off: every held note of the channel releases, as on a DX7
                    // (Dexed matched each held voice's already transposed pitch transposed
                    // again, so with a transpose it released nothing)
                    const unsigned t = perChannel ? controllerTargets(channel) : 1u << ((channel - 1) & 15);
                    for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
                        if ( voices[note].keydown && (t & (1u << ((voices[note].channel - 1) & 15))) ) {
                            voices[note].keydown = false;
                            keyupVoice(note);
                        }
                    }
                    break;
                }
                default:
                    return MidiResult::UnhandledControlChange;
                }
            }
            return MidiResult::Handled;

        case 0xc0 :
            return MidiResult::ProgramChange;

        case 0xd0 :
            if ( !perChannel ) {
                for (auto &x : contexts) { x.ctrl.aftertouch_cc = buf[1]; x.ctrl.refresh(); }
            } else {
                const unsigned t = controllerTargets(channel);
                for (int c = 0; c < MAX_CONTEXTS; c++)
                    for (int ch = 0; ch < 16; ch++)
                        if ( t & (1u << ch) ) { channelCtrl[c][ch].aftertouch_cc = buf[1]; channelCtrl[c][ch].refresh(); }
            }
            return MidiResult::Handled;

		case 0xe0 :
            if ( !perChannel ) {
                for (auto &x : contexts) x.ctrl.pitch_cc = buf[1] | (buf[2] << 7);
            } else {
                const unsigned t = controllerTargets(channel);
                for (int c = 0; c < MAX_CONTEXTS; c++)
                    for (int ch = 0; ch < 16; ch++)
                        if ( t & (1u << ch) ) channelCtrl[c][ch].pitch_cc = buf[1] | (buf[2] << 7);
            }
            return MidiResult::Handled;
        }
    }
    return MidiResult::Handled;
}
