/*
 * Oscairn FM: a native CLAP entry over the headless engine, for Oscairn.
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    "Oscairn FM",
    "Oscairn",
    "https://github.com/oscairn/dexed",
    "",
    "",
    "0.1.0",
    "FM synthesizer based on Dexed",
    features,
};

constexpr const char *stateHeader = "oscairn-fm-state 1";
constexpr size_t maxStateBytes = 1 << 16;

struct Plugin;
Plugin &self(const clap_plugin_t *p);

struct Plugin : SynthEngine::NoteEndSink {
    clap_plugin_t plugin {};
    const clap_host_t *host = nullptr;

    SynthEngine engine;
    uint8_t patch[156];
    // Current values, by index in params(); written by whichever thread owns the engine,
    // read by the main thread.
    std::unique_ptr<std::atomic<double>[]> values;
    bool active = false;
    // The voice name, owned by the main thread (the audio thread has its own copy in patch).
    uint8_t name[10];

    // State loaded while active, handed to the audio thread at the start of a process call.
    std::vector<double> pendingValues;
    uint8_t pendingName[10];
    std::atomic<bool> pendingState { false };

    // During process: where NOTE_END goes and at which frame.
    const clap_output_events_t *out = nullptr;
    uint32_t now = 0;

    Plugin() : values(new std::atomic<double>[params().size()]) {
        std::memcpy(patch, initVoice, sizeof patch);
        std::memcpy(name, initVoice + 145, sizeof name);
        for (size_t i = 0; i < params().size(); i++)
            values[i].store(params()[i].def, std::memory_order_relaxed);
        pendingValues.resize(params().size());
        engine.setTuning(createStandardTuning());
        engine.setPatch(patch);
        engine.setNoteEndSink(this);
        applyAll();
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

    // Sets parameter index i on the engine (on whichever thread owns it).
    void apply(size_t i, double value) {
        const Param &p = params()[i];
        value = std::clamp(value, p.min, p.max);
        if (p.stepped())
            value = std::round(value);
        values[i].store(value, std::memory_order_relaxed);
        Controllers &c = engine.controllers;
        switch (p.kind) {
        case Kind::Byte: {
            const uint8_t b = (uint8_t)((int)value + p.offset);
            if (patch[p.target] == b)
                return;
            patch[p.target] = b;
            if (p.target == 144) // transpose, as Dexed's setDxValue
                engine.panic();
            engine.refreshVoices();
            return;
        }
        case Kind::OpSwitch:
            if (value > 0.5)
                patch[155] |= (uint8_t)(1U << p.target);
            else
                patch[155] &= (uint8_t)~(1U << p.target);
            engine.refreshVoices();
            return;
        case Kind::Setting:
            break;
        }
        // As Dexed's parameter listeners (source/parameter/ParametersMapper.cpp).
        const float v = (float)value;
        const bool on = v > 0.5f;
        auto mod = [&](FmMod &m, int field) {
            if (field == 0) m.range = (int)v;
            else if (field == 1) m.pitch = on;
            else if (field == 2) m.amp = on;
            else m.eg = on;
            c.refresh();
        };
        const Setting s = (Setting)p.target;
        switch (s) {
        case Setting::Engine: engine.setEngineType((int)std::round(v)); break;
        case Setting::Mono: engine.panic(); engine.setMonoMode(on); break;
        case Setting::PortamentoTime:
            c.portamento_cc = (int32_t)(v * 127.0f / 99.0f);
            c.portamento_enable_cc = c.portamento_cc > 0;
            break;
        case Setting::Glissando: c.portamento_gliss_cc = on; break;
        case Setting::PitchBendUp: c.pitch_range_up = (int)v; break;
        case Setting::PitchBendDown: c.pitch_range_dn = (int)v; break;
        case Setting::PitchBendStep: c.pitch_step = (int)v; break;
        case Setting::MasterTune: {
            int32_t tune = ((v / 2 + 0.5) * 0x4000) - 0x2000;
            c.masterTune = ((float)(tune << 11)) * (1.0 / 12);
            break;
        }
        case Setting::Cutoff: engine.fx.uiCutoff = v; break;
        case Setting::Resonance: engine.fx.uiReso = v; break;
        case Setting::Output: engine.fx.uiGain = v; break;
        case Setting::None: break;
        default: {
            const int k = (int)s - (int)Setting::WheelRange;
            FmMod *sources[] = { &c.wheel, &c.foot, &c.breath, &c.at };
            mod(*sources[k / 4], k % 4);
        }
        }
    }

    void applyAll() {
        for (size_t i = 0; i < params().size(); i++)
            apply(i, values[i].load(std::memory_order_relaxed));
    }

    void applyEvent(const clap_event_header_t *h) {
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;
        switch (h->type) {
        case CLAP_EVENT_PARAM_VALUE: {
            auto *e = reinterpret_cast<const clap_event_param_value_t *>(h);
            // per-note values are modulation targets this engine does not have
            if (e->note_id >= 0 || e->key >= 0 || e->channel >= 0 || e->port_index > 0 || !std::isfinite(e->value))
                return;
            const int i = indexOf(e->param_id);
            if (i >= 0)
                apply((size_t)i, e->value);
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

    // Applies a state handed over by the main thread.
    void takePendingState() {
        if (!pendingState.load(std::memory_order_acquire))
            return;
        std::memcpy(patch + 145, pendingName, 10);
        for (size_t i = 0; i < params().size(); i++)
            apply(i, pendingValues[i]);
        engine.panic();
        pendingState.store(false, std::memory_order_release);
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
    s.takePendingState();
    s.out = process->out_events;
    s.now = 0;
    const uint32_t frames = process->frames_count;
    ProcessEvents events(s, process->in_events);
    float *left = nullptr, *right = nullptr;
    if (process->audio_outputs_count > 0 && process->audio_outputs[0].data32 != nullptr) {
        const auto &bus = process->audio_outputs[0];
        left = bus.channel_count > 0 ? bus.data32[0] : nullptr;
        right = bus.channel_count > 1 ? bus.data32[1] : nullptr;
    }
    if (left != nullptr && frames > 0) {
        s.engine.render(left, (int)frames, events);
        if (right != nullptr)
            std::memcpy(right, left, frames * sizeof(float));
    } else {
        events.dispatchUntil(INT32_MAX);
    }
    s.now = frames > 0 ? frames - 1 : 0;
    s.engine.collectEndedNotes();
    s.out = nullptr;
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
    *value = self(plugin).values[i].load(std::memory_order_relaxed);
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

void paramsFlush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *) {
    // [active ? audio thread : main thread]: either way this thread owns the engine now
    Plugin &s = self(plugin);
    for (uint32_t i = 0, n = in->size(in); i < n; i++) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
            s.applyEvent(h);
    }
}

const clap_plugin_params_t paramsExt = { paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush };

// ---- state ----

bool stateSave(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    Plugin &s = self(plugin);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(17);
    out << stateHeader << "\nname";
    char hex[3];
    for (int i = 0; i < 10; i++) {
        std::snprintf(hex, sizeof hex, "%02x", s.name[i]);
        out << (i == 0 ? " " : "") << hex;
    }
    out << "\n";
    for (size_t i = 0; i < params().size(); i++) {
        const double v = s.values[i].load(std::memory_order_relaxed);
        out << "p " << params()[i].id << " ";
        if (params()[i].stepped())
            out << (long)std::lround(v);
        else
            out << v;
        out << "\n";
    }
    const std::string text = out.str();
    for (size_t written = 0; written < text.size();) {
        const int64_t n = stream->write(stream, text.data() + written, text.size() - written);
        if (n <= 0)
            return false;
        written += (size_t)n;
    }
    return true;
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

bool stateLoad(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    Plugin &s = self(plugin);
    std::string text;
    char buffer[4096];
    for (;;) {
        const int64_t n = stream->read(stream, buffer, sizeof buffer);
        if (n < 0)
            return false;
        if (n == 0)
            break;
        text.append(buffer, (size_t)n);
        if (text.size() > maxStateBytes)
            return false;
    }
    std::vector<double> values(params().size());
    uint8_t name[10];
    if (!parseState(text, values, name))
        return false;
    std::memcpy(s.name, name, 10);
    if (!s.active) {
        std::memcpy(s.patch + 145, name, 10);
        for (size_t i = 0; i < values.size(); i++)
            s.apply(i, values[i]);
        return true;
    }
    // Active: hand the state to the audio thread. Wait (briefly) for a previous one to be taken.
    for (int tries = 0; s.pendingState.load(std::memory_order_acquire); tries++) {
        if (tries > 1000)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    s.pendingValues = values; // same size: no allocation
    std::memcpy(s.pendingName, name, 10);
    s.pendingState.store(true, std::memory_order_release);
    if (s.host != nullptr && s.host->request_process != nullptr)
        s.host->request_process(s.host);
    return true;
}

const clap_plugin_state_t stateExt = { stateSave, stateLoad };

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
    s->plugin.activate = [](const clap_plugin_t *p, double rate, uint32_t, uint32_t) {
        Plugin &s = self(p);
        s.engine.prepare(rate);
        s.applyAll(); // prepare resets some controller state; the parameters stay as set
        s.active = true;
        return true;
    };
    s->plugin.deactivate = [](const clap_plugin_t *p) {
        Plugin &s = self(p);
        s.takePendingState();
        s.engine.release();
        s.active = false;
    };
    s->plugin.start_processing = [](const clap_plugin_t *) { return true; };
    s->plugin.stop_processing = [](const clap_plugin_t *) {};
    s->plugin.reset = [](const clap_plugin_t *p) { self(p).engine.panic(); };
    s->plugin.process = process;
    s->plugin.get_extension = [](const clap_plugin_t *, const char *id) -> const void * {
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &paramsExt;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &stateExt;
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPortsExt;
        if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &notePortsExt;
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
    [](const char *) { return true; },
    []() {},
    [](const char *id) -> const void * { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; },
};
