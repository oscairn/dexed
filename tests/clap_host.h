/*
 * A minimal CLAP host for the Oscairn FM tests: loads a plugin, sends events, renders,
 * collects output events.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <clap/clap.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace testhost {

inline void fail(const std::string &message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
}

inline void check(bool ok, const std::string &message) {
    if (!ok)
        fail(message);
}

// Loads clap_entry from a module path.
inline const clap_plugin_entry_t *loadEntry(const char *path) {
#ifdef _WIN32
    fail("module loading is not implemented on Windows");
    return nullptr;
#else
    void *module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        const char *error = dlerror();
        fail(std::string("dlopen ") + path + ": " + (error ? error : "unknown error"));
    }
    auto *entry = static_cast<const clap_plugin_entry_t *>(dlsym(module, "clap_entry"));
    check(entry != nullptr, "no clap_entry");
    check(entry->init(path), "clap_entry init");
    return entry;
#endif
}

// A list of events in time order, as clap_input_events. Fixed-size slots, so pushing into
// reserved room never allocates.
struct EventList {
    using Slot = std::array<uint8_t, 64>;
    std::vector<Slot> events;

    void clear() { events.clear(); }

    void pushBytes(const void *e, size_t size) {
        if (size > sizeof(Slot)) // no std::string here: this runs while processing
            fail("event too large for the test host");
        events.emplace_back();
        std::memcpy(events.back().data(), e, size);
    }

    template <typename T>
    void push(const T &e) { pushBytes(&e, sizeof e); }

    void note(uint16_t type, uint32_t time, int32_t id, int16_t channel, int16_t key, double velocity = 1.0) {
        clap_event_note_t e {};
        e.header = { sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, type, 0 };
        e.note_id = id;
        e.port_index = 0;
        e.channel = channel;
        e.key = key;
        e.velocity = velocity;
        push(e);
    }

    void midi(uint32_t time, const uint8_t *bytes, int size) {
        clap_event_midi_t e {};
        e.header = { sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        e.port_index = 0;
        for (int i = 0; i < size && i < 3; i++)
            e.data[i] = bytes[i];
        push(e);
    }

    void param(uint32_t time, clap_id id, double value) {
        clap_event_param_value_t e {};
        e.header = { sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = id;
        e.cookie = nullptr;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = value;
        push(e);
    }

    static uint32_t size(const clap_input_events_t *list) {
        return (uint32_t) static_cast<const EventList *>(list->ctx)->events.size();
    }
    static const clap_event_header_t *get(const clap_input_events_t *list, uint32_t index) {
        return reinterpret_cast<const clap_event_header_t *>(static_cast<const EventList *>(list->ctx)->events[index].data());
    }
    clap_input_events_t input() const { return { const_cast<EventList *>(this), size, get }; }

    static bool tryPush(const clap_output_events_t *list, const clap_event_header_t *e) {
        auto *self = static_cast<EventList *>(list->ctx);
        // the tests reserve room up front, so this does not allocate while processing
        if (self->events.size() == self->events.capacity() || e->size > sizeof(Slot))
            return false;
        self->pushBytes(e, e->size);
        return true;
    }
    clap_output_events_t output() { return { this, tryPush }; }
};

struct Host {
    clap_host_t host {};
    const clap_plugin_t *plugin = nullptr;
    const clap_plugin_params_t *params = nullptr;
    const clap_plugin_state_t *state = nullptr;
    double rate = 48000;
    uint32_t maxFrames = 0;
    bool active = false;
    std::vector<float> left, right;
    EventList output;

    Host(const clap_plugin_entry_t *entry, const char *id = "org.oscairn.fm") {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "dexed tests";
        host.vendor = "";
        host.url = "";
        host.version = "0";
        host.get_extension = [](const clap_host_t *, const char *) -> const void * { return nullptr; };
        host.request_restart = [](const clap_host_t *) {};
        host.request_process = [](const clap_host_t *) {};
        host.request_callback = [](const clap_host_t *) {};
        auto *factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        check(factory != nullptr, "no plugin factory");
        plugin = factory->create_plugin(factory, &host, id);
        check(plugin != nullptr && plugin->init(plugin), "create plugin");
        params = static_cast<const clap_plugin_params_t *>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
        check(params != nullptr && state != nullptr, "params and state extensions");
    }
    ~Host() {
        if (active) {
            plugin->stop_processing(plugin);
            plugin->deactivate(plugin);
        }
        plugin->destroy(plugin);
    }

    void activate(double sampleRate, uint32_t frames) {
        rate = sampleRate;
        maxFrames = frames;
        left.assign(frames, 0.0f);
        right.assign(frames, 0.0f);
        output.events.reserve(4096);
        check(plugin->activate(plugin, sampleRate, 1, frames), "activate");
        check(plugin->start_processing(plugin), "start processing");
        active = true;
    }

    // Renders frames (<= maxFrames) with the given input events; output events go to output.
    void process(uint32_t frames, const EventList &in) {
        float *channels[2] = { left.data(), right.data() };
        clap_audio_buffer_t out {};
        out.data32 = channels;
        out.channel_count = 2;
        clap_process_t p {};
        p.steady_time = -1;
        p.frames_count = frames;
        p.audio_outputs = &out;
        p.audio_outputs_count = 1;
        const clap_input_events_t input = in.input();
        const clap_output_events_t outEvents = output.output();
        p.in_events = &input;
        p.out_events = &outEvents;
        check(plugin->process(plugin, &p) == CLAP_PROCESS_CONTINUE, "process");
    }

    std::string saveState() {
        std::string data;
        clap_ostream_t stream { &data, [](const clap_ostream_t *s, const void *buffer, uint64_t size) -> int64_t {
                                   static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buffer), size);
                                   return (int64_t)size;
                               } };
        check(state->save(plugin, &stream), "state save");
        return data;
    }

    bool loadState(const std::string &data) {
        struct Reader { const std::string *data; size_t pos; } reader { &data, 0 };
        clap_istream_t stream { &reader, [](const clap_istream_t *s, void *buffer, uint64_t size) -> int64_t {
                                   auto *r = static_cast<Reader *>(s->ctx);
                                   const size_t n = std::min<size_t>(size, r->data->size() - r->pos);
                                   std::memcpy(buffer, r->data->data() + r->pos, n);
                                   r->pos += n;
                                   return (int64_t)n;
                               } };
        return state->load(plugin, &stream);
    }
};

} // namespace testhost
