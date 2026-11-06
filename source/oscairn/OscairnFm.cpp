/*
 * Octet: a native CLAP entry over the headless engine, for Oscairn.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * No JUCE: the plugin drives SynthEngine from the raw CLAP events. Notes keep their note
 * ID for the voice's whole life and end with NOTE_END; there is no note output; parameters
 * have fixed IDs (Parameters.h); state is a versioned text format, the same bytes on every
 * platform. Events are applied on the engine's 64-sample control grid, so the output does
 * not depend on the host's block size.
 */
#include <clap/clap.h>

#include "engine/SynthEngine.h"
#include "Parameters.h"
#include "Presets.h"

#include <oscairn-clap/sound-contexts.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <functional>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace oscairn_fm;

namespace {

const char *const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr };

const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    "org.oscairn.fm",
    "Octet",
    "Oscairn",
    "https://github.com/oscairn/dexed",
    "",
    "",
    "0.2.2",
    "FM synthesizer based on Dexed",
    features,
};

constexpr const char *stateHeader = "oscairn-fm-state 1";
constexpr const char *snapshotHeader = "octet-contexts 1";
constexpr size_t maxStateBytes = 1 << 16;
constexpr int contextCount = SynthEngine::MAX_CONTEXTS;
constexpr int queueSize = 32;

struct Plugin;
Plugin &self(const clap_plugin_t *p);

// A prepared sound (RFC 0005). Slot 0 is the protected default sound, ID 0.
struct Slot {
    std::atomic<bool> ready { false }; // prepared (main thread writes, audio reads)
    uint32_t id = 0;                   // main thread
    char name[64] = {};                // main thread
    std::atomic<uint32_t> queued { 0 }; // accepted selections not yet applied
    std::atomic<uint32_t> generation { 0 }; // bumped by every preparation
    uint32_t loadedGeneration = 0;     // audio thread: which preparation the engine holds
    uint8_t patch[156];                // the engine's voice data once loaded
    ExtendedVoice ext;                 // the engine's Extended values once loaded
    uint8_t voiceName[10];             // main thread
    std::vector<double> prepared;      // values from prepare, loaded by the audio thread
};

struct Selection { int slot; int16_t channel; }; // slot -1: choke_retiring's request

struct Plugin : SynthEngine::NoteEndSink {
    clap_plugin_t plugin {};
    const clap_host_t *host = nullptr;

    SynthEngine engine;
    Slot slots[contextCount];
    // Values by context and parameter index: written by whichever thread owns the engine,
    // read by the main thread.
    std::unique_ptr<std::atomic<double>[]> values;
    std::atomic<int> view { 0 }; // the context selected on channel 1: what get_value shows
    // Parameters whose new value the host has not been sent since the sound shown changed
    // (audio thread); a full output queue keeps them for the next process or flush.
    std::vector<uint8_t> feedbackPending;
    size_t feedbackCount = 0;
    bool active = false;
    uint32_t nextId = 1;

    // Selections: single producer (main), single consumer (audio).
    Selection queue[queueSize];
    std::atomic<uint32_t> queueHead { 0 }, queueTail { 0 };

    // A state loaded while active: the main thread writes the context's values (which
    // get_value and save read at once) and sets the flag; the audio thread re-applies that
    // context's values to the engine at its next process or flush. A later load just writes
    // again and sets the flag again, so the last one wins; nothing waits.
    std::atomic<bool> stateChanged[contextCount];

    // During process: where NOTE_END and parameter feedback go, and at which frame.
    const clap_output_events_t *out = nullptr;
    // The host's ring-out buffer for the next process() (oscairn.sound-snapshots draft 3), or null.
    float *const *ringOut = nullptr;
    uint32_t now = 0;

    std::atomic<double> &value(int c, size_t i) { return values[(size_t)c * params().size() + i]; }

    Plugin() : values(new std::atomic<double>[contextCount * params().size()]), feedbackPending(params().size(), 0) {
        for (int c = 0; c < contextCount; c++) {
            std::memcpy(slots[c].patch, initVoice, 156);
            slots[c].ext = extDefaults();
            std::memcpy(slots[c].voiceName, initVoice + 145, 10);
            slots[c].prepared.assign(params().size(), 0.0);
            for (size_t i = 0; i < params().size(); i++)
                value(c, i).store(params()[i].def, std::memory_order_relaxed);
            engine.setPatch(c, slots[c].patch);
            engine.setExtended(c, &slots[c].ext);
        }
        std::snprintf(slots[0].name, sizeof slots[0].name, "Default");
        slots[0].ready.store(true, std::memory_order_release);
        for (auto &f : stateChanged)
            f.store(false, std::memory_order_relaxed);
        engine.setTuning(createStandardTuning());
        engine.setNoteEndSink(this);
        engine.setPerChannelControllers(true); // RFC 0005: controllers act per MIDI channel
        for (int c = 0; c < contextCount; c++)
            applyAll(c);
    }

    void noteEnded(const NoteRef &note) override {
        if (out == nullptr)
            return;
        clap_event_note_t e {};
        e.header = { sizeof e, now, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0 };
        e.note_id = note.id;
        e.port_index = note.port;
        e.channel = note.channel;
        e.key = note.key;
        e.velocity = 0;
        out->try_push(out, &e.header);
    }

    // Sets parameter index i of context c on the engine (on whichever thread owns it).
    void apply(int c, size_t i, double v) {
        const Param &p = params()[i];
        v = std::clamp(v, p.min, p.max);
        if (p.stepped())
            v = std::round(v);
        value(c, i).store(v, std::memory_order_relaxed);
        uint8_t *patch = slots[c].patch;
        SynthContext &x = engine.context(c);
        Controllers &ctrl = x.ctrl;
        switch (p.kind) {
        case Kind::Byte: {
            const uint8_t b = (uint8_t)((int)v + p.offset);
            if (patch[p.target] == b)
                return;
            patch[p.target] = b;
            if (p.target == 144) // transpose, as Dexed's setDxValue
                engine.panicContext(c);
            engine.refreshVoices(c);
            return;
        }
        case Kind::OpSwitch:
            if (v > 0.5)
                patch[155] |= (uint8_t)(1U << p.target);
            else
                patch[155] &= (uint8_t)~(1U << p.target);
            engine.refreshVoices(c);
            return;
        case Kind::Ext: {
            int16_t &e = slots[c].ext.v[p.target];
            if (e == (int16_t)v)
                return;
            e = (int16_t)v;
            if (p.target == ext::ProfileSel) // voices of the other profile cannot continue
                engine.panicContext(c);
            engine.refreshVoices(c);
            return;
        }
        case Kind::Setting:
            break;
        }
        // As Dexed's parameter listeners (source/parameter/ParametersMapper.cpp).
        const float f = (float)v;
        const bool on = f > 0.5f;
        auto mod = [&](FmMod &m, int field) {
            if (field == 0) m.range = (int)f;
            else if (field == 1) m.pitch = on;
            else if (field == 2) m.amp = on;
            else m.eg = on;
            ctrl.refresh();
        };
        const Setting s = (Setting)p.target;
        switch (s) {
        case Setting::Engine: engine.setEngineType(c, (int)std::round(f)); break;
        case Setting::Mono: engine.panicContext(c); engine.setMonoMode(c, on); break;
        case Setting::PortamentoTime: {
            const int32_t time = (int32_t)(f * 127.0f / 99.0f);
            engine.setPortamento(c, time, time > 0);
            return;
        }
        case Setting::Glissando: ctrl.portamento_gliss_cc = on; break;
        case Setting::PitchBendUp: ctrl.pitch_range_up = (int)f; break;
        case Setting::PitchBendDown: ctrl.pitch_range_dn = (int)f; break;
        case Setting::PitchBendStep: ctrl.pitch_step = (int)f; break;
        case Setting::MasterTune: {
            int32_t tune = ((f / 2 + 0.5) * 0x4000) - 0x2000;
            ctrl.masterTune = ((float)(tune * 2048)) * (1.0 / 12); // Dexed's tune << 11, defined for negative tunes
            break;
        }
        case Setting::Cutoff: x.fx.uiCutoff = f; break;
        case Setting::Resonance: x.fx.uiReso = f; break;
        case Setting::Output: x.fx.uiGain = f; break;
        case Setting::None: break;
        default: {
            const int k = (int)s - (int)Setting::WheelRange;
            FmMod *sources[] = { &ctrl.wheel, &ctrl.foot, &ctrl.breath, &ctrl.at };
            mod(*sources[k / 4], k % 4);
        }
        }
        engine.syncChannelControllers(c); // the voices read per-channel copies
    }

    void applyAll(int c) {
        for (size_t i = 0; i < params().size(); i++)
            apply(c, i, value(c, i).load(std::memory_order_relaxed));
    }

    // Loads a prepared slot into the engine (audio thread, or main thread when inactive or
    // exclusive), with the default sound's current controller values.
    void load(int c) {
        Slot &slot = slots[c];
        const uint32_t generation = slot.generation.load(std::memory_order_acquire);
        if (slot.loadedGeneration == generation)
            return;
        slot.loadedGeneration = generation;
        engine.panicContext(c);
        Controllers &ctrl = engine.context(c).ctrl;
        const Controllers &now = engine.context(0).ctrl;
        ctrl.pitch_cc = now.pitch_cc;
        ctrl.modwheel_cc = now.modwheel_cc;
        ctrl.foot_cc = now.foot_cc;
        ctrl.breath_cc = now.breath_cc;
        ctrl.aftertouch_cc = now.aftertouch_cc;
        ctrl.portamento_enable_cc = now.portamento_enable_cc;
        ctrl.mpeEnabled = now.mpeEnabled;
        std::memcpy(slot.patch, initVoice, 156); // (the voice name stays with the main thread)
        slot.ext = extDefaults();
        for (size_t i = 0; i < params().size(); i++)
            apply(c, i, slot.prepared[i]);
        engine.resetLfo(c);
    }

    // Applies the queued selections (audio thread, at the start of a process call).
    void takeSelections() {
        const uint32_t head = queueHead.load(std::memory_order_acquire);
        uint32_t tail = queueTail.load(std::memory_order_relaxed);
        const int before = engine.selectedContext(0);
        if (tail == head)
            return;
        for (uint32_t t = tail; t != head; t++) {
            const Selection &s = queue[t % queueSize];
            if (s.slot < 0) { // before the selections queued after it (draft 3)
                engine.chokeRetiring();
                continue;
            }
            load(s.slot);
            engine.selectContext(s.slot, s.channel);
        }
        // publish the new selections before releasing the queue's pins, so a discard that
        // sees no pin also sees the selection
        engine.publishContextState();
        for (; tail != head; tail++)
            if (queue[tail % queueSize].slot >= 0)
                slots[queue[tail % queueSize].slot].queued.fetch_sub(1, std::memory_order_release);
        queueTail.store(tail, std::memory_order_release);
        const int after = engine.selectedContext(0);
        if (after != before) {
            view.store(after, std::memory_order_relaxed);
            // parameter feedback for the sound now shown: the values that differ from the last one's
            for (size_t i = 0; i < params().size(); i++)
                if (!feedbackPending[i] && value(after, i).load(std::memory_order_relaxed) != value(before, i).load(std::memory_order_relaxed)) {
                    feedbackPending[i] = 1;
                    feedbackCount++;
                }
        }
    }

    // Sends the pending parameter feedback, the shown sound's current values, until the output
    // queue is full; the rest stay pending.
    void sendFeedback() {
        const int c = view.load(std::memory_order_relaxed);
        for (size_t i = 0; out != nullptr && feedbackCount > 0 && i < params().size(); i++) {
            if (!feedbackPending[i])
                continue;
            clap_event_param_value_t e {};
            e.header = { sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
            e.param_id = params()[i].id;
            e.note_id = e.port_index = e.channel = e.key = -1;
            e.value = value(c, i).load(std::memory_order_relaxed);
            if (!out->try_push(out, &e.header))
                return;
            feedbackPending[i] = 0;
            feedbackCount--;
        }
    }

    void applyEvent(const clap_event_header_t *h) {
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;
        switch (h->type) {
        case CLAP_EVENT_PARAM_VALUE: {
            auto *e = reinterpret_cast<const clap_event_param_value_t *>(h);
            // per-note values are modulation targets this engine does not have
            if (e->note_id >= 0 || e->key >= 0 || e->channel > 15 || e->port_index > 0 || !std::isfinite(e->value))
                return;
            const int i = indexOf(e->param_id);
            if (i < 0)
                return;
            // the sounds selected for the event's channel (all channels: every selected sound)
            bool done[contextCount] = {};
            for (int ch = 0; ch < 16; ch++) {
                if (e->channel >= 0 && ch != e->channel)
                    continue;
                const int c = engine.selectedContext(ch);
                if (!done[c]) {
                    done[c] = true;
                    apply(c, (size_t)i, e->value);
                }
            }
            return;
        }
        case CLAP_EVENT_NOTE_ON: {
            auto *e = reinterpret_cast<const clap_event_note_t *>(h);
            if (e->key < 0 || e->key > 127 || e->port_index > 0)
                return;
            const int channel = e->channel >= 0 && e->channel < 16 ? e->channel : 0;
            const int velocity = std::clamp((int)std::lround(e->velocity * 127.0), 1, 127);
            NoteRef ref;
            ref.id = e->note_id;
            ref.port = 0;
            ref.channel = (int16_t)channel;
            ref.key = e->key;
            engine.keydown((uint8_t)(channel + 1), (uint8_t)e->key, (uint8_t)velocity, ref);
            return;
        }
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            auto *e = reinterpret_cast<const clap_event_note_t *>(h);
            if (e->port_index > 0)
                return;
            NoteRef match;
            match.id = e->note_id;
            match.channel = e->channel;
            match.key = e->key;
            if (h->type == CLAP_EVENT_NOTE_OFF)
                engine.releaseNotes(match);
            else
                engine.chokeNotes(match);
            return;
        }
        case CLAP_EVENT_MIDI: {
            auto *e = reinterpret_cast<const clap_event_midi_t *>(h);
            if (e->port_index > 0 || e->data[0] < 0x80 || e->data[0] >= 0xF0)
                return;
            // program changes and unmapped controllers have no meaning here
            engine.processMidiMessage(e->data, (e->data[0] & 0x0f) + 1);
            return;
        }
        default:
            return;
        }
    }

    // Applies states loaded while active (audio thread, or flush).
    void takeStateChanges() {
        for (int c = 0; c < contextCount; c++) {
            if (!stateChanged[c].exchange(false, std::memory_order_acquire))
                continue;
            applyAll(c);
            engine.panicContext(c);
        }
    }

    // What process does before a block's events: selections, then loaded states, then the
    // parameter feedback still to send.
    void beforeEvents() {
        takeSelections();
        takeStateChanges();
        sendFeedback();
    }
};

Plugin &self(const clap_plugin_t *p) { return *static_cast<Plugin *>(p->plugin_data); }

// Feeds a process call's input events to the engine as it reaches their frames.
struct ProcessEvents : SynthEngine::EventSource {
    Plugin &plugin;
    const clap_input_events_t *in;
    uint32_t count, next = 0;
    ProcessEvents(Plugin &p, const clap_input_events_t *in) : plugin(p), in(in), count(in ? in->size(in) : 0) {}
    void dispatchUntil(int samplePos) override {
        for (; next < count; next++) {
            const clap_event_header_t *h = in->get(in, next);
            if ((int)h->time > samplePos)
                return;
            plugin.now = h->time;
            plugin.applyEvent(h);
        }
    }
};

clap_process_status process(const clap_plugin_t *p, const clap_process_t *process) {
    Plugin &s = self(p);
    s.out = process->out_events;
    s.now = 0;
    s.beforeEvents(); // before this call's events (RFC 0005)
    if (process->transport != nullptr && (process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0)
        s.engine.setTempo(process->transport->tempo);
    const uint32_t frames = process->frames_count;
    ProcessEvents events(s, process->in_events);
    float *left = nullptr, *right = nullptr;
    if (process->audio_outputs_count > 0 && process->audio_outputs[0].data32 != nullptr) {
        const auto &bus = process->audio_outputs[0];
        left = bus.channel_count > 0 ? bus.data32[0] : nullptr;
        right = bus.channel_count > 1 ? bus.data32[1] : nullptr;
    }
    if (left != nullptr && frames > 0) {
        s.engine.render(left, right, (int)frames, events, s.ringOut);
    } else {
        events.dispatchUntil(INT32_MAX);
    }
    s.now = frames > 0 ? frames - 1 : 0;
    s.engine.collectEndedNotes();
    s.engine.publishContextState();
    s.out = nullptr;
    s.ringOut = nullptr; // given for one process() only
    return CLAP_PROCESS_CONTINUE;
}

// ---- params ----

uint32_t paramsCount(const clap_plugin_t *) { return (uint32_t)params().size(); }

bool paramsInfo(const clap_plugin_t *, uint32_t index, clap_param_info_t *info) {
    if (index >= params().size())
        return false;
    const Param &p = params()[index];
    *info = {};
    info->id = p.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (p.stepped())
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (p.choices != nullptr)
        info->flags |= CLAP_PARAM_IS_ENUM;
    info->min_value = p.min;
    info->max_value = p.max;
    info->default_value = p.def;
    std::snprintf(info->name, sizeof info->name, "%s", p.name.c_str());
    std::snprintf(info->module, sizeof info->module, "%s", p.module.c_str());
    return true;
}

bool paramsValue(const clap_plugin_t *plugin, clap_id id, double *value) {
    const int i = indexOf(id);
    if (i < 0)
        return false;
    Plugin &s = self(plugin);
    *value = s.value(s.view.load(std::memory_order_relaxed), (size_t)i).load(std::memory_order_relaxed);
    return true;
}

bool paramsToText(const clap_plugin_t *, clap_id id, double value, char *text, uint32_t size) {
    const int i = indexOf(id);
    if (i < 0 || !std::isfinite(value) || size == 0)
        return false;
    const Param &p = params()[i];
    value = std::clamp(value, p.min, p.max);
    const int n = (int)std::lround(value);
    static const char *const notes[] = { "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#" };
    switch (p.text) {
    case Text::Number: std::snprintf(text, size, "%d", n); break;
    case Text::Signed: std::snprintf(text, size, n == 0 ? "%d" : "%+d", n); break;
    case Text::Choice:
    case Text::Switch: std::snprintf(text, size, "%s", p.choices[n - (int)p.min]); break;
    case Text::Note: std::snprintf(text, size, "%s%d", notes[n % 12], (n + 9) / 12 - 1); break; // Dexed's break point labels
    case Text::Key: std::snprintf(text, size, "%s%d", notes[(n + 3) % 12], n / 12 - 2); break; // MIDI note, C-2 = 0
    case Text::Percent: std::snprintf(text, size, "%.1f %%", value * 100); break;
    case Text::Tune: std::snprintf(text, size, "%+.1f cents", value * 100); break;
    }
    return true;
}

bool paramsFromText(const clap_plugin_t *, clap_id id, const char *text, double *value) {
    const int i = indexOf(id);
    if (i < 0 || text == nullptr)
        return false;
    const Param &p = params()[i];
    if (p.choices != nullptr)
        for (int k = 0; p.choices[k] != nullptr; k++)
            if (std::strcmp(text, p.choices[k]) == 0) {
                *value = p.min + k;
                return true;
            }
    if (p.text == Text::Note) {
        static const char *const notes[] = { "A#", "C#", "D#", "F#", "G#", "A", "B", "C", "D", "E", "F", "G" };
        static const int semis[] = { 1, 4, 6, 9, 11, 0, 2, 3, 5, 7, 8, 10 }; // from A
        for (int k = 0; k < 12; k++) {
            const size_t len = std::strlen(notes[k]);
            if (std::strncmp(text, notes[k], len) == 0) {
                char *end = nullptr;
                const long octave = std::strtol(text + len, &end, 10);
                if (end == text + len || *end != 0)
                    return false;
                // value v has label notes[v % 12] (from A) and octave (v + 9) / 12 - 1
                for (int v = 0; v <= 99; v++)
                    if (v % 12 == semis[k] && (v + 9) / 12 - 1 == octave) {
                        *value = v;
                        return true;
                    }
                return false;
            }
        }
        return false;
    }
    if (p.text == Text::Key) {
        char buf[8];
        for (int v = (int)p.min; v <= (int)p.max; v++)
            if (paramsToText(nullptr, id, v, buf, sizeof buf) && std::strcmp(buf, text) == 0) {
                *value = v;
                return true;
            }
        return false;
    }
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double v;
    if (!(in >> v) || !std::isfinite(v))
        return false;
    if (p.text == Text::Percent || p.text == Text::Tune)
        v /= 100;
    if (v < p.min || v > p.max)
        return false;
    *value = v;
    return true;
}

void paramsFlush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out) {
    // [active ? audio thread : main thread]: either way this thread owns the engine now
    Plugin &s = self(plugin);
    if (s.active) {
        // as process does: queued selections and loaded states first (RFC 0005)
        s.out = out;
        s.now = 0;
        s.beforeEvents();
        s.out = nullptr;
    }
    for (uint32_t i = 0, n = in->size(in); i < n; i++) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
            s.applyEvent(h);
    }
}

const clap_plugin_params_t paramsExt = { paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush };

// ---- state ----

// The patch text of a sound: voice name and every parameter, by ID.
std::string patchText(const uint8_t name[10], const std::function<double(size_t)> &valueOf) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(17);
    out << stateHeader << "\nname";
    char hex[3];
    for (int i = 0; i < 10; i++) {
        std::snprintf(hex, sizeof hex, "%02x", name[i]);
        out << (i == 0 ? " " : "") << hex;
    }
    out << "\n";
    for (size_t i = 0; i < params().size(); i++) {
        const double v = valueOf(i);
        out << "p " << params()[i].id << " ";
        if (params()[i].stepped())
            out << (long)std::lround(v);
        else
            out << v;
        out << "\n";
    }
    return out.str();
}

bool writeAll(const clap_ostream_t *stream, const std::string &text) {
    for (size_t written = 0; written < text.size();) {
        const int64_t n = stream->write(stream, text.data() + written, text.size() - written);
        if (n <= 0)
            return false;
        written += (size_t)n;
    }
    return true;
}

bool readAll(const clap_istream_t *stream, std::string &text, size_t limit) {
    char buffer[4096];
    for (;;) {
        const int64_t n = stream->read(stream, buffer, sizeof buffer);
        if (n < 0)
            return false;
        if (n == 0)
            return true;
        text.append(buffer, (size_t)n);
        if (text.size() > limit)
            return false;
    }
}

bool stateSave(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    Plugin &s = self(plugin);
    const int c = s.view.load(std::memory_order_relaxed);
    return writeAll(stream, patchText(s.slots[c].voiceName, [&](size_t i) { return s.value(c, i).load(std::memory_order_relaxed); }));
}

// Parses a whole state; false (and nothing changed) on anything unexpected.
bool parseState(const std::string &text, std::vector<double> &values, uint8_t name[10]) {
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(in, line) || line != stateHeader)
        return false;
    for (size_t i = 0; i < params().size(); i++)
        values[i] = params()[i].def;
    std::memcpy(name, initVoice + 145, 10);
    std::vector<bool> seen(params().size(), false);
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        std::istringstream ls(line);
        ls.imbue(std::locale::classic());
        std::string tag;
        ls >> tag;
        if (tag == "name") {
            std::string hex;
            if (!(ls >> hex) || hex.size() != 20)
                return false;
            for (int i = 0; i < 10; i++) {
                if (!std::isxdigit((unsigned char)hex[2 * i]) || !std::isxdigit((unsigned char)hex[2 * i + 1]))
                    return false;
                const unsigned long b = std::strtoul(hex.substr(2 * i, 2).c_str(), nullptr, 16);
                if (b > 127)
                    return false;
                name[i] = (uint8_t)b;
            }
        } else if (tag == "p") {
            uint32_t id;
            double v;
            if (!(ls >> id >> v) || !std::isfinite(v))
                return false;
            const int i = indexOf(id);
            if (i < 0 || seen[i])
                return false;
            const Param &p = params()[i];
            if (v < p.min || v > p.max || (p.stepped() && v != std::round(v)))
                return false;
            values[i] = v;
            seen[i] = true;
        } else {
            return false;
        }
        std::string rest;
        if (ls >> rest)
            return false;
    }
    return true;
}

void setSound(Plugin &s, const std::vector<double> &values, const uint8_t name[10]);

bool stateLoad(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Plugin &s = self(plugin);
    std::string text;
    if (!readAll(stream, text, maxStateBytes))
        return false;
    std::vector<double> values(params().size());
    uint8_t name[10];
    if (!parseState(text, values, name))
        return false;
    setSound(s, values, name);
    return true;
}

// Makes values and name the sound shown (the context selected on channel 1), as a state load.
void setSound(Plugin &s, const std::vector<double> &values, const uint8_t name[10]) {
    const int c = s.view.load(std::memory_order_relaxed);
    std::memcpy(s.slots[c].voiceName, name, 10);
    if (!s.active) {
        std::memcpy(s.slots[c].patch + 145, name, 10);
        for (size_t i = 0; i < values.size(); i++)
            s.apply(c, i, values[i]);
        return;
    }
    // Active: the values are visible to get_value and save at once; the audio thread applies
    // them to the engine at its next process or flush.
    for (size_t i = 0; i < values.size(); i++)
        s.value(c, i).store(values[i], std::memory_order_relaxed);
    s.stateChanged[c].store(true, std::memory_order_release);
    if (s.host != nullptr && s.host->request_process != nullptr)
        s.host->request_process(s.host);
}

const clap_plugin_state_t stateExt = { stateSave, stateLoad };

// ---- preset load: a cartridge voice into the sound shown ----

const clap_host_preset_load_t *hostPresetLoad(const clap_host_t *host) {
    if (host == nullptr || host->get_extension == nullptr)
        return nullptr;
    auto *ext = static_cast<const clap_host_preset_load_t *>(host->get_extension(host, CLAP_EXT_PRESET_LOAD));
    return ext != nullptr ? ext : static_cast<const clap_host_preset_load_t *>(host->get_extension(host, CLAP_EXT_PRESET_LOAD_COMPAT));
}

bool presetLoad(const clap_plugin_t *plugin, uint32_t kind, const char *location, const char *loadKey) {
    Plugin &s = self(plugin);
    std::vector<double> values;
    uint8_t name[10];
    std::string error;
    const clap_host_preset_load_t *host = hostPresetLoad(s.host);
    if (!readPreset(kind, location, loadKey, values, name, error)) {
        if (host != nullptr && host->on_error != nullptr)
            host->on_error(s.host, kind, location, loadKey, 0, error.c_str());
        return false;
    }
    setSound(s, values, name);
    if (host != nullptr && host->loaded != nullptr)
        host->loaded(s.host, kind, location, loadKey);
    return true;
}

const clap_plugin_preset_load_t presetLoadExt = { presetLoad };

// ---- sound contexts (RFC 0005) ----

uint32_t contextsCapacity(const clap_plugin_t *) { return contextCount; }

bool contextsGet(const clap_plugin_t *plugin, uint32_t index, oscairn_sound_context_info_t *info) {
    Plugin &s = self(plugin);
    if (index >= (uint32_t)contextCount || !s.slots[index].ready.load(std::memory_order_acquire))
        return false;
    const Slot &slot = s.slots[index];
    const SynthContext &x = s.engine.context((int)index);
    *info = {};
    info->id = slot.id;
    std::snprintf(info->name, sizeof info->name, "%s", slot.name);
    info->selected_channels = x.selectedMask.load(std::memory_order_relaxed);
    info->active_voices = x.activeVoices.load(std::memory_order_relaxed);
    info->idle = x.silent.load(std::memory_order_acquire) && slot.queued.load(std::memory_order_acquire) == 0;
    return true;
}

void diagnostic(char *message, uint32_t capacity, const char *text) {
    if (message != nullptr && capacity > 0)
        std::snprintf(message, capacity, "%s", text);
}

int freeSlot(Plugin &s, int after = 0) {
    for (int c = after + 1; c < contextCount; c++)
        if (!s.slots[c].ready.load(std::memory_order_acquire))
            return c;
    return -1;
}

// Makes free slot c a prepared sound with a new ID (main thread).
uint32_t publish(Plugin &s, int c, const char *name, const std::vector<double> &values, const uint8_t voiceName[10]) {
    Slot &slot = s.slots[c];
    // free: the audio thread does not touch this slot until it is selected
    slot.prepared = values; // same size: no allocation
    std::memcpy(slot.voiceName, voiceName, 10);
    std::snprintf(slot.name, sizeof slot.name, "%s", name);
    slot.id = s.nextId++;
    slot.generation.fetch_add(1, std::memory_order_release);
    slot.ready.store(true, std::memory_order_release);
    if (!s.active) // nothing renders: load now, so get_value and state see it at once
        s.load(c);
    return slot.id;
}

uint32_t contextsPrepare(const clap_plugin_t *plugin, const char *name, const oscairn_sound_parameter_t *supplied, uint32_t count,
                         const char *patch, const oscairn_asset_ref_t *base, char *message, uint32_t capacity) {
    Plugin &s = self(plugin);
    if (name == nullptr || std::strlen(name) > 63) {
        diagnostic(message, capacity, "name missing or longer than 63 bytes");
        return CLAP_INVALID_ID;
    }
    if (base != nullptr) {
        diagnostic(message, capacity, "this instrument has no assets");
        return CLAP_INVALID_ID;
    }
    if (count > 0 && supplied == nullptr) {
        diagnostic(message, capacity, "parameters missing");
        return CLAP_INVALID_ID;
    }
    std::vector<double> values(params().size());
    uint8_t voiceName[10];
    for (size_t i = 0; i < values.size(); i++)
        values[i] = params()[i].def;
    std::memcpy(voiceName, initVoice + 145, 10);
    if (patch != nullptr && !parseState(patch, values, voiceName)) {
        diagnostic(message, capacity, "patch text is not an oscairn-fm-state 1 patch, or has a bad line");
        return CLAP_INVALID_ID;
    }
    std::vector<bool> seen(values.size(), false);
    for (uint32_t k = 0; k < count; k++) {
        const int i = indexOf(supplied[k].id);
        const double v = supplied[k].value;
        if (i < 0 || seen[i]) {
            diagnostic(message, capacity, i < 0 ? "unknown parameter ID" : "parameter given twice");
            return CLAP_INVALID_ID;
        }
        const Param &p = params()[i];
        if (!std::isfinite(v) || v < p.min || v > p.max || (p.stepped() && v != std::round(v))) {
            diagnostic(message, capacity, "parameter value out of range");
            return CLAP_INVALID_ID;
        }
        seen[i] = true;
        values[i] = v;
    }
    const int c = freeSlot(s);
    if (c < 0) {
        diagnostic(message, capacity, "no free sound context");
        return CLAP_INVALID_ID;
    }
    return publish(s, c, name, values, voiceName);
}

// prepare_preset (draft 5): a context from a cartridge voice, as prepare from its values.
uint32_t contextsPreparePreset(const clap_plugin_t *plugin, const char *name, uint32_t kind, const char *location, const char *loadKey,
                               char *message, uint32_t capacity) {
    if (name == nullptr || std::strlen(name) > 63) {
        diagnostic(message, capacity, "name missing or longer than 63 bytes");
        return CLAP_INVALID_ID;
    }
    std::vector<double> values;
    uint8_t voiceName[10];
    std::string error;
    if (!readPreset(kind, location, loadKey, values, voiceName, error)) {
        diagnostic(message, capacity, error.c_str());
        return CLAP_INVALID_ID;
    }
    Plugin &s = self(plugin);
    const int c = freeSlot(s);
    if (c < 0) {
        diagnostic(message, capacity, "no free sound context");
        return CLAP_INVALID_ID;
    }
    return publish(s, c, name, values, voiceName);
}

int slotOf(Plugin &s, uint32_t id) {
    for (int c = 0; c < contextCount; c++)
        if (s.slots[c].ready.load(std::memory_order_acquire) && s.slots[c].id == id)
            return c;
    return -1;
}

// Queues a selection (main thread), or choke_retiring's request as slot -1; false when the
// queue is full.
bool enqueue(Plugin &s, int c, int16_t channel) {
    const uint32_t head = s.queueHead.load(std::memory_order_relaxed);
    if (head - s.queueTail.load(std::memory_order_acquire) >= (uint32_t)queueSize)
        return false; // queue full
    if (c >= 0)
        s.slots[c].queued.fetch_add(1, std::memory_order_relaxed);
    s.queue[head % queueSize] = { c, channel };
    s.queueHead.store(head + 1, std::memory_order_release);
    if (!s.active) {
        // nothing renders: apply at once
        s.takeSelections();
        s.engine.publishContextState();
    } else if (s.host != nullptr && s.host->request_process != nullptr) {
        s.host->request_process(s.host);
    }
    return true;
}

bool contextsSelect(const clap_plugin_t *plugin, uint32_t id, int16_t channel) {
    Plugin &s = self(plugin);
    const int c = slotOf(s, id);
    if (c < 0 || channel < -1 || channel > 15)
        return false;
    return enqueue(s, c, channel);
}

bool contextsDiscard(const clap_plugin_t *plugin, uint32_t id) {
    Plugin &s = self(plugin);
    const int c = slotOf(s, id);
    if (c <= 0) // unknown, or the protected default sound
        return false;
    Slot &slot = s.slots[c];
    const SynthContext &x = s.engine.context(c);
    // queued is checked before the engine's view: the audio thread publishes a selection
    // before it releases the queue entry
    if (slot.queued.load(std::memory_order_acquire) != 0 || !x.silent.load(std::memory_order_acquire) ||
        x.selectedMask.load(std::memory_order_relaxed) != 0 || x.activeVoices.load(std::memory_order_relaxed) != 0)
        return false;
    slot.ready.store(false, std::memory_order_release);
    return true;
}

// The exclusive snapshot: every sound, and the sound each channel plays.
bool snapshotSave(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    Plugin &s = self(plugin);
    s.takeSelections(); // accepted selections belong to the snapshot
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << snapshotHeader << "\n";
    for (int c = 0; c < contextCount; c++) {
        if (!s.slots[c].ready.load(std::memory_order_acquire))
            continue;
        const std::string text = patchText(s.slots[c].voiceName, [&](size_t i) { return s.value(c, i).load(std::memory_order_relaxed); });
        std::string hexName;
        char hex[3];
        for (const char *n = s.slots[c].name; *n; n++) {
            std::snprintf(hex, sizeof hex, "%02x", (unsigned char)*n);
            hexName += hex;
        }
        size_t lines = 0;
        for (char ch : text)
            lines += ch == '\n';
        out << "context " << c << " " << (hexName.empty() ? "-" : hexName) << " " << lines << "\n" << text;
    }
    out << "channels";
    for (int ch = 0; ch < 16; ch++)
        out << " " << s.engine.selectedContext(ch);
    out << "\n";
    return writeAll(stream, out.str());
}

// A snapshot's sounds, read and validated: touches nothing of the plugin, so it is safe on the
// main thread while rendering.
struct Loaded { bool used = false; std::string name; std::vector<double> values; uint8_t voiceName[10]; };

bool readSnapshot(const clap_istream_t *stream, Loaded (&loaded)[contextCount], int (&channels)[16]) {
    std::string text;
    if (!readAll(stream, text, maxStateBytes * contextCount))
        return false;
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(in, line) || line != snapshotHeader)
        return false;
    bool haveChannels = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        ls.imbue(std::locale::classic());
        std::string tag;
        ls >> tag;
        if (tag == "context") {
            int c;
            std::string hexName;
            size_t lines;
            if (!(ls >> c >> hexName >> lines) || c < 0 || c >= contextCount || loaded[c].used || lines > 1000)
                return false;
            std::string patch;
            for (size_t k = 0; k < lines; k++) {
                if (!std::getline(in, line))
                    return false;
                patch += line + "\n";
            }
            Loaded &l = loaded[c];
            l.used = true;
            if (hexName != "-") {
                if (hexName.size() % 2 || hexName.size() > 126)
                    return false;
                for (size_t k = 0; k < hexName.size(); k += 2) {
                    if (!std::isxdigit((unsigned char)hexName[k]) || !std::isxdigit((unsigned char)hexName[k + 1]))
                        return false;
                    l.name += (char)std::strtoul(hexName.substr(k, 2).c_str(), nullptr, 16);
                }
            }
            l.values.resize(params().size());
            if (!parseState(patch, l.values, l.voiceName))
                return false;
        } else if (tag == "channels") {
            for (int ch = 0; ch < 16; ch++)
                if (!(ls >> channels[ch]) || channels[ch] < 0 || channels[ch] >= contextCount)
                    return false;
            haveChannels = true;
        } else if (!tag.empty()) {
            return false;
        }
    }
    if (!loaded[0].used || !haveChannels)
        return false;
    for (int ch = 0; ch < 16; ch++)
        if (!loaded[channels[ch]].used)
            return false;
    return true;
}

bool snapshotLoad(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Plugin &s = self(plugin);
    Loaded loaded[contextCount];
    int channels[16];
    if (!readSnapshot(stream, loaded, channels))
        return false;
    // replace: exclusive, so this thread owns the engine; every voice ends (reported at the next
    // process), every sound is new, with a fresh ID
    s.engine.panic();
    for (int c = 0; c < contextCount; c++) {
        Slot &slot = s.slots[c];
        slot.ready.store(false, std::memory_order_release);
        slot.queued.store(0, std::memory_order_relaxed);
        if (!loaded[c].used)
            continue;
        slot.prepared = loaded[c].values;
        std::memcpy(slot.voiceName, loaded[c].voiceName, 10);
        std::snprintf(slot.name, sizeof slot.name, "%s", loaded[c].name.c_str());
        slot.id = c == 0 ? 0 : s.nextId++;
        slot.generation.fetch_add(1, std::memory_order_release);
        slot.ready.store(true, std::memory_order_release);
        s.load(c);
    }
    s.queueTail.store(s.queueHead.load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (int ch = 0; ch < 16; ch++)
        s.engine.selectContext(channels[ch], ch);
    s.view.store(s.engine.selectedContext(0), std::memory_order_relaxed);
    s.engine.publishContextState();
    return true;
}

// oscairn.sound-snapshots: every sound of a snapshot prepared as a new context beside the playing
// ones (main thread, while rendering), all or nothing; nothing is selected. A sound's engine state is
// loaded at its selection, as for prepare; the engine's tables are shared by every context already.
bool snapshotsPrepare(const clap_plugin_t *plugin, const clap_istream_t *stream, uint32_t *selected, char *message, uint32_t capacity) {
    Plugin &s = self(plugin);
    if (stream == nullptr || selected == nullptr) {
        diagnostic(message, capacity, "invalid arguments");
        return false;
    }
    Loaded loaded[contextCount];
    int channels[16];
    if (!readSnapshot(stream, loaded, channels)) {
        diagnostic(message, capacity, "not an octet-contexts 1 snapshot, or a bad line");
        return false;
    }
    int slot[contextCount], found = -1;
    for (int c = 0; c < contextCount; c++)
        if (loaded[c].used && (slot[c] = found = freeSlot(s, found)) < 0) {
            diagnostic(message, capacity, "no free sound contexts for the snapshot");
            return false;
        }
    uint32_t ids[contextCount] = {};
    for (int c = 0; c < contextCount; c++)
        if (loaded[c].used)
            ids[c] = publish(s, slot[c], loaded[c].name.c_str(), loaded[c].values, loaded[c].voiceName);
    for (int ch = 0; ch < 16; ch++)
        selected[ch] = ids[channels[ch]];
    return true;
}

const clap_plugin_state_t snapshotExt = { snapshotSave, snapshotLoad };
const oscairn_plugin_sound_contexts_t contextsExt = { contextsCapacity, contextsGet, contextsPrepare, contextsSelect, contextsDiscard, &snapshotExt,
                                                    contextsPreparePreset };
// ring_out (draft 3): audio thread, just before a process(); sounds no channel selects render into
// its first two channels, those choke_retiring is fading out into its last two.
void snapshotsRingOut(const clap_plugin_t *plugin, const clap_audio_buffer_t *buffer) {
    bool valid = buffer != nullptr && buffer->channel_count >= 4 && buffer->data32 != nullptr;
    for (int k = 0; valid && k < 4; k++)
        valid = buffer->data32[k] != nullptr;
    self(plugin).ringOut = valid ? buffer->data32 : nullptr;
}

// choke_retiring (draft 3): main thread, queued like a selection; at that boundary every sound
// retiring then fades out, into the ring-out buffer's last two channels until it is silent.
bool snapshotsChokeRetiring(const clap_plugin_t *plugin) { return enqueue(self(plugin), -1, -1); }

const oscairn_plugin_sound_snapshots_t snapshotsExt = { snapshotsPrepare, snapshotsRingOut, snapshotsChokeRetiring };

// ---- ports ----

uint32_t audioPortsCount(const clap_plugin_t *, bool isInput) { return isInput ? 0 : 1; }

bool audioPortsGet(const clap_plugin_t *, uint32_t index, bool isInput, clap_audio_port_info_t *info) {
    if (isInput || index != 0)
        return false;
    *info = {};
    info->id = 0;
    std::snprintf(info->name, sizeof info->name, "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t audioPortsExt = { audioPortsCount, audioPortsGet };

uint32_t notePortsCount(const clap_plugin_t *, bool isInput) { return isInput ? 1 : 0; }

bool notePortsGet(const clap_plugin_t *, uint32_t index, bool isInput, clap_note_port_info_t *info) {
    if (!isInput || index != 0)
        return false;
    *info = {};
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof info->name, "Notes");
    return true;
}

const clap_plugin_note_ports_t notePortsExt = { notePortsCount, notePortsGet };

// ---- plugin ----

const clap_plugin_t *create(const clap_host_t *host) {
    auto *s = new Plugin;
    s->host = host;
    s->plugin.desc = &descriptor;
    s->plugin.plugin_data = s;
    s->plugin.init = [](const clap_plugin_t *) { return true; };
    s->plugin.destroy = [](const clap_plugin_t *p) { delete &self(p); };
    s->plugin.activate = [](const clap_plugin_t *p, double rate, uint32_t, uint32_t maxFrames) {
        Plugin &s = self(p);
        s.engine.prepare(rate, (int)maxFrames);
        for (int c = 0; c < contextCount; c++)
            s.applyAll(c); // prepare resets some controller state; the parameters stay as set
        s.active = true;
        return true;
    };
    s->plugin.deactivate = [](const clap_plugin_t *p) {
        Plugin &s = self(p);
        s.takeStateChanges();
        s.engine.release();
        s.active = false;
    };
    s->plugin.start_processing = [](const clap_plugin_t *) { return true; };
    s->plugin.stop_processing = [](const clap_plugin_t *) {};
    s->plugin.reset = [](const clap_plugin_t *p) {
        Plugin &s = self(p);
        s.engine.reset();
        for (int c = 0; c < contextCount; c++)
            s.applyAll(c); // as activate: the parameters stay as set
    };
    s->plugin.process = process;
    s->plugin.get_extension = [](const clap_plugin_t *, const char *id) -> const void * {
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &paramsExt;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &stateExt;
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPortsExt;
        if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &notePortsExt;
        if (std::strcmp(id, OSCAIRN_EXT_SOUND_CONTEXTS) == 0) return &contextsExt;
        if (std::strcmp(id, OSCAIRN_EXT_SOUND_SNAPSHOTS) == 0) return &snapshotsExt;
        if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0 || std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0) return &presetLoadExt;
        return nullptr;
    };
    s->plugin.on_main_thread = [](const clap_plugin_t *) {};
    return &s->plugin;
}

const clap_plugin_factory_t factory = {
    [](const clap_plugin_factory_t *) -> uint32_t { return 1; },
    [](const clap_plugin_factory_t *, uint32_t index) -> const clap_plugin_descriptor_t * { return index == 0 ? &descriptor : nullptr; },
    [](const clap_plugin_factory_t *, const clap_host_t *host, const char *id) -> const clap_plugin_t * {
        return std::strcmp(id, descriptor.id) == 0 ? create(host) : nullptr;
    },
};

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char *path) {
        setPluginPath(path);
        return true;
    },
    []() {},
    [](const char *id) -> const void * {
        if (std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0)
            return &factory;
        if (std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0 || std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0)
            return presetDiscoveryFactory();
        return nullptr;
    },
};
