/*
 * Tests of the Octet CLAP (source/oscairn).
 *
 *   oscairn-fm-test baseline FILE [--reverse] [--module PATH]   the DX7 cases, as the baseline
 *   oscairn-fm-test baseline-write FILE [--module PATH]
 *   oscairn-fm-test two-rates FILE [--module PATH]   two instances at different rates in turn
 *   oscairn-fm-test params FILE [--write]            parameter IDs and metadata never change
 *   oscairn-fm-test notes                            note IDs, wildcards, NOTE_END, stealing
 *   oscairn-fm-test state                            round trip, active load, rejected states
 *   oscairn-fm-test no-alloc                         process never allocates
 *   oscairn-fm-test threads                          main-thread calls while processing (TSan)
 *   oscairn-fm-test topology                         the 32 algorithm graphs, against renders
 *   oscairn-fm-test contexts                         native sound contexts (RFC 0005)
 *   oscairn-fm-test state-active                     state loads and flush while active, with contexts
 *
 * oscairn-fm-test needs --module; oscairn-fm-test-linked (OSCAIRN_FM_LINKED) has the plugin
 * linked in, which no-alloc needs.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "clap_host.h"
#include "dx7_cases.h"
#include "baseline_file.h"
#include "oscairn/Parameters.h"
#include "msfa/algorithms.h"
#include "msfa/ext_filter.h"
#include "msfa/ext_note.h"
#include "msfa/ext_tables.h"
#include "msfa/ext_waves.h"
#include "msfa/ext_waves_data.h"
#include "msfa/lfo.h"
#include "msfa/synth.h"
#include <oscairn-clap/sound-contexts.h>

#include <algorithm>
#include <array>
#include <complex>
#include <functional>
#include <atomic>
#include <cmath>
#include <fstream>
#include <new>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <chrono>
#include <unistd.h>

#ifdef OSCAIRN_FM_LINKED
extern "C" const clap_plugin_entry_t clap_entry;
#endif

using namespace testhost;
using namespace dx7cases;

#include "allocation_guard.h"

namespace {

#ifdef OSCAIRN_FM_LINKED
const clap_plugin_entry_t *entry = &clap_entry;
#else
const clap_plugin_entry_t *entry = nullptr;
#endif

// ---- the DX7 cases through the CLAP ----

// Voice and settings as parameter events (a DX7 voice is exactly the Byte and switch parameters).
void voiceAndSettings(EventList &e, const Case &c) {
    const auto v = voice(c.voiceIndex);
    for (const auto &p : oscairn_fm::params()) {
        if (p.kind == oscairn_fm::Kind::Byte)
            e.param(0, p.id, (double)v[p.target] - p.offset);
        else if (p.kind == oscairn_fm::Kind::OpSwitch)
            e.param(0, p.id, 1);
    }
    const Settings &s = c.settings;
    e.param(0, 100, s.engine);
    e.param(0, 101, s.mono ? 1 : 0);
    e.param(0, 102, s.portamentoTime);
    e.param(0, 110, s.wheelRange);
    e.param(0, 111, s.wheelPitch ? 1 : 0);
    e.param(0, 112, s.wheelAmp ? 1 : 0);
    e.param(0, 122, s.atRange);
    e.param(0, 125, s.atEgBias ? 1 : 0);
    e.param(0, 104, s.pitchUp);
    e.param(0, 105, s.pitchDown);
}

struct CaseRun {
    const Case &c;
    Host host;
    EventList in;
    std::vector<Event> evs;
    int total, pos = 0;
    size_t next = 0;
    Hash hash;

    explicit CaseRun(const Case &c) : c(c), host(entry), evs(events(c.scenario)) {
        host.activate(c.rate, (uint32_t)c.block);
        // Warm-up block, as the plugin harness: the voice arrives, nothing plays.
        voiceAndSettings(in, c);
        host.process((uint32_t)c.block, in);
        total = (int)std::lround(scenarioSeconds(c.scenario) * c.rate);
        in.events.reserve(64);
    }

    bool step() {
        if (pos >= total)
            return false;
        const int n = std::min(c.block, total - pos);
        in.clear();
        for (; next < evs.size() && eventSample(evs[next], c.rate) < pos + n; next++)
            in.midi((uint32_t)(eventSample(evs[next], c.rate) - pos), evs[next].bytes, evs[next].size);
        host.output.clear();
        host.process((uint32_t)n, in);
        hash.add(host.left.data(), n);
        hash.add(host.right.data(), n);
        pos += n;
        return true;
    }
};

uint64_t renderCase(const Case &c) {
    CaseRun r(c);
    while (r.step()) {}
    return r.hash.h;
}

std::map<std::string, uint64_t> readBaseline(const char *file) {
    std::map<std::string, uint64_t> expected;
    std::ifstream in(file);
    check((bool)in, std::string("cannot read ") + file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string name, hex;
        ls >> name >> hex;
        expected[name] = std::strtoull(hex.c_str(), nullptr, 16);
    }
    return expected;
}

int twoRates(const char *file) {
    auto expected = readBaseline(file);
    const auto all = cases();
    int pairs = 0, failures = 0;
    for (const auto &a : all) {
        if (a.rate == 48000)
            continue;
        const Case &b = all[pairs % 120];
        CaseRun ra(a), rb(b);
        for (bool more = true; more;) {
            more = ra.step();
            more = rb.step() || more;
        }
        for (const CaseRun *r : { &ra, &rb })
            if (expected[r->c.name] != r->hash.h) {
                std::fprintf(stderr, "DIFF %s (two instances)\n", r->c.name.c_str());
                failures++;
            }
        pairs++;
    }
    std::printf("%d pairs, %d differ\n", pairs, failures);
    return failures == 0 ? 0 : 1;
}

// ---- parameters ----

int paramsTest(const char *file, bool write) {
    Host h(entry);
    const uint32_t count = h.params->count(h.plugin);
    std::ostringstream listing;
    listing << "# Octet parameters: id | module | name | min | max | default | flags. IDs must never change.\n";
    std::set<clap_id> ids;
    for (uint32_t i = 0; i < count; i++) {
        clap_param_info_t info;
        check(h.params->get_info(h.plugin, i, &info), "get_info");
        check(ids.insert(info.id).second, "duplicate id " + std::to_string(info.id));
        check(std::isfinite(info.min_value) && std::isfinite(info.max_value) && info.min_value <= info.default_value &&
                  info.default_value <= info.max_value, "range of " + std::to_string(info.id));
        check(info.module[0] != 0 && info.name[0] != 0, "module and name of " + std::to_string(info.id));
        double value;
        check(h.params->get_value(h.plugin, info.id, &value) && value == info.default_value, "initial value of " + std::to_string(info.id));
        // value text round trip
        const bool stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        const int steps = stepped ? (int)(info.max_value - info.min_value) : 20;
        for (int k = 0; k <= steps; k++) {
            const double v = stepped ? info.min_value + k : info.min_value + (info.max_value - info.min_value) * k / steps;
            char text[256];
            check(h.params->value_to_text(h.plugin, info.id, v, text, sizeof text) && text[0] != 0, "value_to_text " + std::to_string(info.id));
            double back;
            check(h.params->text_to_value(h.plugin, info.id, text, &back), std::string("text_to_value ") + text);
            check(stepped ? back == v : std::fabs(back - v) < 1e-3, std::string("text round trip ") + text);
        }
        char line[512];
        std::snprintf(line, sizeof line, "%u | %s | %s | %g | %g | %g | %s%s\n", info.id, info.module, info.name, info.min_value,
                      info.max_value, info.default_value, stepped ? "stepped" : "continuous",
                      (info.flags & CLAP_PARAM_IS_ENUM) ? " enum" : "");
        listing << line;
    }
    if (write) {
        std::ofstream(file) << listing.str();
        std::printf("wrote %u parameters to %s\n", count, file);
        return 0;
    }
    std::ifstream in(file);
    std::stringstream expected;
    expected << in.rdbuf();
    if (expected.str() != listing.str()) {
        std::fprintf(stderr, "parameter list differs from %s:\n%s", file, listing.str().c_str());
        return 1;
    }
    std::printf("%u parameters, unique IDs, valid ranges, text round trips, unchanged list\n", count);
    return 0;
}

// ---- notes ----

struct NoteEnd { uint32_t time; int32_t id; int16_t channel, key; };

std::vector<NoteEnd> ends(const Host &h) {
    std::vector<NoteEnd> r;
    for (const auto &slot : h.output.events) {
        auto *e = reinterpret_cast<const clap_event_note_t *>(slot.data());
        if (e->header.type == CLAP_EVENT_NOTE_END)
            r.push_back({ e->header.time, e->note_id, e->channel, e->key });
    }
    return r;
}

int notesTest() {
    Host h(entry);
    h.activate(48000, 256);
    EventList in;
    auto run = [&](int blocks) {
        std::vector<NoteEnd> all;
        for (int b = 0; b < blocks; b++) {
            h.output.clear();
            h.process(256, in);
            in.clear();
            for (auto e : ends(h))
                all.push_back(e);
        }
        return all;
    };
    auto has = [](const std::vector<NoteEnd> &v, int32_t id) {
        for (auto &e : v)
            if (e.id == id)
                return true;
        return false;
    };

    // Two notes on one key keep apart by ID: releasing 7 ends 7 only.
    in.note(CLAP_EVENT_NOTE_ON, 0, 7, 0, 60);
    in.note(CLAP_EVENT_NOTE_ON, 10, 8, 0, 60);
    check(run(20).empty(), "no NOTE_END while both notes are held");
    in.note(CLAP_EVENT_NOTE_OFF, 0, 7, 0, 60);
    auto e = run(200);
    check(has(e, 7) && !has(e, 8), "NOTE_END for the released note 7 only");
    for (auto &x : e)
        check(x.key == 60 && x.channel == 0, "NOTE_END carries the note's channel and key");

    // Choke fades a held note out within 3 ms (no click), then frees it and ends it.
    {
        std::vector<float> after;
        std::vector<NoteEnd> chokeEnds;
        in.note(CLAP_EVENT_NOTE_CHOKE, 100, 8, 0, 60);
        for (int b = 0; b < 2; b++) {
            h.output.clear();
            h.process(256, in);
            in.clear();
            after.insert(after.end(), h.left.begin(), h.left.begin() + 256);
            for (auto x : ends(h))
                chokeEnds.push_back(x);
        }
        // the choke lands on the 64-sample grid (frame 128); 3 ms at 48 kHz is 145 samples
        float before = 0, tail = 0, jump = 0;
        for (int i = 100; i < 128; i++)
            before = std::max(before, std::fabs(after[i]));
        for (int i = 128 + 150; i < 512; i++)
            tail = std::max(tail, std::fabs(after[i]));
        for (int i = 129; i < 128 + 145; i++)
            jump = std::max(jump, std::fabs(after[i] - after[i - 1]));
        std::printf("choke: level before %g, largest step during the fade %g, largest after it %g\n", before, jump, tail);
        // after the fade only the output DC filter's small decaying offset is left
        check(before > 0 && tail < 0.01f * before, "the choked note fades to silence within 3 ms");
        check(jump < before, "no step as large as the note itself (no click)");
        check(chokeEnds.size() == 1 && chokeEnds[0].id == 8, "choke gives NOTE_END for 8 once its fade is over");
    }

    // Wildcard note-off releases every held note.
    for (int k = 0; k < 4; k++)
        in.note(CLAP_EVENT_NOTE_ON, 0, 20 + k, 0, 64 + k);
    run(4);
    in.note(CLAP_EVENT_NOTE_OFF, 0, -1, -1, -1);
    e = run(200);
    for (int k = 0; k < 4; k++)
        check(has(e, 20 + k), "wildcard release ends note " + std::to_string(20 + k));

    // Seventeen held notes on 16 voices: the stolen note ends when stolen.
    for (int k = 0; k < 16; k++)
        in.note(CLAP_EVENT_NOTE_ON, 0, 100 + k, 0, 40 + k);
    check(run(4).empty(), "no NOTE_END with 16 held notes");
    h.output.clear();
    in.note(CLAP_EVENT_NOTE_ON, 50, 200, 0, 90);
    h.process(256, in);
    in.clear();
    e = ends(h);
    check(e.size() == 1 && e[0].id >= 100 && e[0].id < 116 && e[0].time == 50, "one stolen note ends at frame 50");

    // Sustain (MIDI CC 64) holds released notes until the pedal is up.
    in.note(CLAP_EVENT_NOTE_OFF, 0, -1, -1, -1);
    run(200);
    const uint8_t pedalDown[] = { 0xB0, 64, 127 }, pedalUp[] = { 0xB0, 64, 0 };
    in.midi(0, pedalDown, 3);
    in.note(CLAP_EVENT_NOTE_ON, 0, 300, 0, 72);
    in.note(CLAP_EVENT_NOTE_OFF, 100, 300, 0, 72);
    check(!has(run(100), 300), "a sustained note does not end");
    in.midi(0, pedalUp, 3);
    check(has(run(200), 300), "the note ends after the pedal is up");

    // MIDI-dialect notes end too, with note ID -1 and their channel and key.
    const uint8_t on[] = { 0x93, 61, 100 }, off[] = { 0x83, 61, 0 };
    in.midi(0, on, 3);
    in.midi(200, off, 3);
    e = run(200);
    check(e.size() == 1 && e[0].id == -1 && e[0].channel == 3 && e[0].key == 61, "MIDI note ends with its channel and key");

    {
        // a choked voice is free again: with 16 held notes, choke one, and a new note takes
        // the choked voice instead of stealing a held one
        Host f(entry);
        f.activate(48000, 256);
        EventList fin;
        for (int k = 0; k < 16; k++)
            fin.note(CLAP_EVENT_NOTE_ON, 0, 400 + k, 0, 40 + k);
        f.process(256, fin);
        fin.clear();
        fin.note(CLAP_EVENT_NOTE_CHOKE, 0, 405, 0, -1);
        for (int b = 0; b < 4; b++) {
            f.output.clear();
            f.process(256, fin);
            fin.clear();
        }
        fin.note(CLAP_EVENT_NOTE_ON, 0, 500, 0, 90);
        f.output.clear();
        f.process(256, fin);
        check(ends(f).empty(), "a new note reuses the choked voice; no held note is stolen");
    }
    {
        // CC 123 releases every held note of its channel, with a voice transpose too (Dexed
        // transposed the held pitch twice and released nothing)
        Host t(entry);
        t.activate(48000, 256);
        EventList tin;
        tin.param(0, 4, 5); // transpose +5
        const uint8_t a[] = { 0x90, 60, 100 }, b[] = { 0x90, 64, 100 }, other[] = { 0x91, 67, 100 }, allOff[] = { 0xB0, 123, 0 };
        tin.midi(0, a, 3);
        tin.midi(0, b, 3);
        tin.midi(0, other, 3);
        t.process(256, tin);
        tin.clear();
        tin.midi(0, allOff, 3);
        std::set<int> released;
        for (int k = 0; k < 200; k++) {
            t.output.clear();
            t.process(256, tin);
            tin.clear();
            for (auto &x : ends(t))
                released.insert(x.key);
        }
        check(released == std::set<int>{ 60, 64 }, "CC 123 releases channel 1's held notes despite a transpose, not channel 2's");
    }
    std::printf("note IDs, choke, wildcards, stealing, sustain and MIDI notes give the expected NOTE_END\n");
    return 0;
}

// ---- state ----

Case stateCase() { return cases()[7]; } // a poly case at 48 kHz, block 512

int stateTest() {
    const Case c = stateCase();
    // A voice set through parameters, saved.
    std::string saved;
    {
        CaseRun r(c);
        saved = r.host.saveState();
        // Save, load into a fresh instance, save again: the same bytes.
        Host other(entry);
        check(other.loadState(saved), "load a saved state");
        check(other.saveState() == saved, "save after load gives the same bytes");
    }
    // Render after load equals render after parameters.
    const uint64_t viaParams = renderCase(c);
    {
        Host h(entry);
        check(h.loadState(saved), "load before activation");
        h.activate(c.rate, (uint32_t)c.block);
        EventList in;
        h.process((uint32_t)c.block, in); // the same warm-up block, without events
        const auto evs = events(c.scenario);
        const int total = (int)std::lround(scenarioSeconds(c.scenario) * c.rate);
        Hash hash;
        size_t next = 0;
        for (int pos = 0; pos < total; pos += c.block) {
            const int n = std::min(c.block, total - pos);
            in.clear();
            for (; next < evs.size() && eventSample(evs[next], c.rate) < pos + n; next++)
                in.midi((uint32_t)(eventSample(evs[next], c.rate) - pos), evs[next].bytes, evs[next].size);
            h.process((uint32_t)n, in);
            hash.add(h.left.data(), n);
            hash.add(h.right.data(), n);
        }
        check(hash.h == viaParams, "render after state load equals render after parameter events");
    }
    // Loading while active is taken at the next process call.
    {
        Host h(entry);
        h.activate(48000, 512);
        check(h.loadState(saved), "load while active");
        EventList none;
        h.process(512, none);
        check(h.saveState() == saved, "state loaded while active is in effect after one process call");
    }
    // Bad states are refused and change nothing.
    {
        Host h(entry);
        const std::string before = h.saveState();
        std::string wrongVersion = saved;
        wrongVersion.replace(0, std::strlen("oscairn-fm-state 1"), "oscairn-fm-state 9");
        std::string outOfRange = saved;
        outOfRange += "p 1 33\n";
        std::string unknownId = saved + "p 999 1\n";
        std::string notStepped = "oscairn-fm-state 1\np 2 1.5\n";
        for (const std::string *bad : { &wrongVersion, &outOfRange, &unknownId, &notStepped })
            check(!h.loadState(*bad), "bad state refused");
        check(!h.loadState(""), "empty state refused");
        check(h.saveState() == before, "refused states change nothing");
    }
    std::printf("state round trip, render after load, load while active and refused states pass\n");
    return 0;
}

// ---- real-time ----

int noAllocTest() {
#ifndef OSCAIRN_FM_LINKED
    fail("no-alloc needs oscairn-fm-test-linked");
#endif
    // Every scenario (notes, stealing, mono, MPE, controllers), parameter edits, a state
    // load taken while active, and NOTE_END output.
    for (const Case &c : { cases()[0], cases()[121], cases()[122], cases().back() }) {
        Host h(entry);
        h.activate(c.rate, (uint32_t)c.block);
        EventList in;
        in.events.reserve(512);
        voiceAndSettings(in, c);
        const std::string state = h.saveState();
        check(h.loadState(state), "load while active");
        {
            // flush while active: applies the loaded state, then the parameter
            EventList fin, fout;
            fin.param(0, 1003, 50);
            fout.events.reserve(512);
            const clap_input_events_t input = fin.input();
            const clap_output_events_t output = fout.output();
            forbidAllocation = true;
            h.params->flush(h.plugin, &input, &output);
            forbidAllocation = false;
        }
        forbidAllocation = true;
        h.process((uint32_t)c.block, in);
        forbidAllocation = false;
        const auto evs = events(c.scenario);
        const int total = (int)std::lround(scenarioSeconds(c.scenario) * c.rate);
        size_t next = 0;
        for (int pos = 0; pos < total; pos += c.block) {
            const int n = std::min(c.block, total - pos);
            in.clear();
            for (; next < evs.size() && eventSample(evs[next], c.rate) < pos + n; next++)
                in.midi((uint32_t)(eventSample(evs[next], c.rate) - pos), evs[next].bytes, evs[next].size);
            in.note(CLAP_EVENT_NOTE_ON, 0, pos, 0, 50 + (pos / c.block) % 30);
            in.param(1, 1003, (pos / c.block) % 99);
            h.output.clear();
            forbidAllocation = true;
            h.process((uint32_t)n, in);
            forbidAllocation = false;
        }
    }
    {
        // sound contexts: selections loaded and applied inside process, several sounds playing
        Host h(entry);
        h.activate(48000, 512);
        auto *ctx = static_cast<const oscairn_plugin_sound_contexts_t *>(h.plugin->get_extension(h.plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        const oscairn_sound_parameter_t b[] = { { 1019, 2 }, { 1017, 90 } };
        const uint32_t ids[] = { ctx->prepare(h.plugin, "A", nullptr, 0, nullptr, nullptr, nullptr, 0),
                                 ctx->prepare(h.plugin, "B", b, 2, nullptr, nullptr, nullptr, 0) };
        EventList in;
        in.events.reserve(64);
        for (int k = 0; k < 40; k++) {
            ctx->select(h.plugin, ids[k % 2], (int16_t)(k % 3 == 0 ? -1 : k % 16));
            in.clear();
            in.note(CLAP_EVENT_NOTE_ON, 0, k, (int16_t)(k % 16), 40 + k);
            in.note(CLAP_EVENT_NOTE_OFF, 300, k - 3, -1, -1);
            in.param(10, 1017, 50 + k);
            h.output.clear();
            forbidAllocation = true;
            h.process(512, in);
            forbidAllocation = false;
        }
    }
    check(forbiddenAllocations.load() == 0, std::to_string(forbiddenAllocations.load()) + " allocations while processing");
    std::printf("no allocation while processing\n");
    return 0;
}

int threadsTest() {
    Host h(entry);
    h.activate(48000, 128);
    const std::string state = h.saveState();
    std::atomic<bool> done { false };
    std::thread audio([&] {
        EventList in;
        in.events.reserve(64);
        for (int b = 0; !done.load(); b++) {
            in.clear();
            in.note(CLAP_EVENT_NOTE_ON, 0, b, 0, 40 + b % 40);
            in.note(CLAP_EVENT_NOTE_OFF, 64, b, 0, 40 + b % 40);
            in.param(10, 1017, b % 99);
            h.output.clear();
            h.process(128, in);
        }
    });
    // concurrent prepare, select and discard (RFC 0005: main thread, during processing)
    auto *ctx = static_cast<const oscairn_plugin_sound_contexts_t *>(h.plugin->get_extension(h.plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
    uint32_t prepared = 0;
    for (int i = 0; i < 300; i++) {
        for (uint32_t k = 1; k < ctx->capacity(h.plugin); k++) {
            oscairn_sound_context_info_t info;
            if (ctx->get(h.plugin, k, &info) && info.idle)
                ctx->discard(h.plugin, info.id);
        }
        const oscairn_sound_parameter_t p[] = { { 1019, (double)(i % 4) } };
        const uint32_t id = ctx->prepare(h.plugin, "Concurrent", p, 1, nullptr, nullptr, nullptr, 0);
        if (id != CLAP_INVALID_ID) {
            prepared++;
            ctx->select(h.plugin, id, (int16_t)(i % 2 ? -1 : i % 16));
        } else {
            ctx->select(h.plugin, 0, -1);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    check(prepared > 7, "concurrent preparation reuses discarded contexts");
    for (int i = 0; i < 200; i++) {
        double v;
        h.params->get_value(h.plugin, 1017, &v);
        char text[64];
        h.params->value_to_text(h.plugin, 1017, v, text, sizeof text);
        const std::string s = h.saveState();
        check(h.loadState(i % 2 ? state : s), "load while processing");
    }
    done = true;
    audio.join();
    std::printf("main-thread parameter reads, saves, loads and context prepare/select/discard while processing (%u prepared)\n", prepared);
    return 0;
}

// ---- algorithm topology ----

// Renders note 60 with only the operators in `on` at full level (simple ratio-1 sine
// operators with instant envelopes), with the given algorithm, feedback and engine.
// engine 3: the Extended profile (8 operators, the 88 algorithms)
std::vector<float> renderOperators(int algorithm, unsigned on, int feedback, int engine) {
    Host h(entry);
    h.activate(48000, 512);
    EventList in;
    in.events.reserve(512);
    if (engine == 3) {
        in.param(0, 5, 1); // profile Extended
        in.param(0, 40, algorithm);
        in.param(0, 41, feedback);
        for (int n = 1; n <= 8; n++) {
            const uint32_t id = 1000 * n;
            for (int k = 41; k <= 44; k++)
                in.param(0, id + k, 99); // levels
            for (int k = 45; k <= 48; k++)
                in.param(0, id + k, 0);  // times
            in.param(0, id + 51, (on >> (n - 1)) & 1 ? 99 : 0);
            in.param(0, id + 32, 1);     // coarse 1 (ratio 1)
            in.param(0, id + 33, 0);
            in.param(0, id + 34, 0);
            in.param(0, id + 57, 0);     // velocity sensitivity
        }
    }
    in.param(0, 1, engine == 3 ? 1 : algorithm);
    in.param(0, 2, engine == 3 ? 0 : feedback);
    in.param(0, 100, engine == 3 ? 0 : engine);
    for (int n = 1; n <= 6; n++) {
        const uint32_t id = 1000 * n;
        for (int k = 1; k <= 4; k++)
            in.param(0, id + k, 99); // rates
        for (int k = 5; k <= 8; k++)
            in.param(0, id + k, 99); // levels
        in.param(0, id + 16, 0);     // velocity sensitivity
        in.param(0, id + 17, (on >> (n - 1)) & 1 ? 99 : 0);
        in.param(0, id + 19, 1);     // coarse 1 (ratio 1)
        in.param(0, id + 20, 0);
        in.param(0, id + 21, 0);
    }
    h.process(512, in);
    in.clear();
    in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 60, 0.8);
    std::vector<float> out;
    for (int b = 0; b < 4; b++) {
        h.process(512, in);
        in.clear();
        out.insert(out.end(), h.left.begin(), h.left.begin() + 512);
    }
    return out;
}

bool audible(const std::vector<float> &x) {
    for (float v : x)
        if (std::fabs(v) > 1e-3f)
            return true;
    return false;
}

// Checks the renders of every algorithm of a set against its graph: which operators are heard
// alone, which modulate which, and that feedback acts. engines: Classic's three, or 3 for
// Extended; n: operators per voice.
int checkTopology(const OperatorGraph *graphs, int count, int n, const std::vector<int> &engines) {
    int failures = 0;
    auto expect = [&](bool ok, int alg, int engine, const std::string &what) {
        if (!ok) {
            std::fprintf(stderr, "algorithm %d, engine %d: %s\n", alg, engine, what.c_str());
            failures++;
        }
    };
    for (int a = 1; a <= count; a++) {
        const OperatorGraph &g = graphs[a - 1];
        // ancestors[k]: every operator whose output reaches operator k
        unsigned ancestors[8];
        for (int k = 0; k < n; k++) {
            unsigned m = g.inputs[k], prev = 0;
            while (m != prev) {
                prev = m;
                for (int j = 0; j < n; j++)
                    if (m & (1u << j))
                        m |= g.inputs[j];
            }
            ancestors[k] = m;
        }
        for (int engine : engines) {
            std::vector<float> alone[8];
            for (int k = 0; k < n; k++) {
                alone[k] = renderOperators(a, 1u << k, 0, engine);
                const bool carrier = (g.carriers >> k) & 1;
                expect(audible(alone[k]) == carrier, a, engine, "operator " + std::to_string(k + 1) + (carrier ? " should" : " should not") + " be heard alone");
            }
            // Every edge: with operator m and everything it reaches on (its path to the
            // output), adding operator j changes that path iff j directly modulates an
            // operator on it; otherwise j is just heard beside it (or not at all).
            for (int m = 0; m < n; m++) {
                unsigned path = 1u << m, modulators = 0;
                for (int k = 0; k < n; k++)
                    if ((ancestors[k] >> m) & 1)
                        path |= 1u << k;
                for (int k = 0; k < n; k++)
                    if ((path >> k) & 1)
                        modulators |= g.inputs[k];
                const auto base = renderOperators(a, path, 0, engine);
                for (int j = 0; j < n; j++) {
                    if ((path >> j) & 1)
                        continue;
                    const bool modulates = (modulators >> j) & 1;
                    const auto both = renderOperators(a, path | (1u << j), 0, engine);
                    // without modulation: the path plus j alone (up to per-voice rounding)
                    float worst = 0;
                    for (size_t i = 0; i < both.size(); i++)
                        worst = std::max(worst, std::fabs(both[i] - (base[i] + alone[j][i])));
                    expect((worst > 1e-3f) == modulates, a, engine,
                           "operator " + std::to_string(j + 1) + (modulates ? " should" : " should not") +
                               " modulate the path of operator " + std::to_string(m + 1));
                }
            }
            // Feedback acts on the operator it goes into: with that operator and everything it
            // reaches on, feedback changes the sound.
            const int to = g.feedbackTo - 1;
            unsigned path = 1u << to;
            for (int k = 0; k < n; k++)
                if ((ancestors[k] >> to) & 1)
                    path |= 1u << k;
            path |= 1u << (g.feedbackFrom - 1);
            expect(renderOperators(a, path, 7, engine) != renderOperators(a, path, 0, engine), a, engine, "feedback changes nothing");
        }
    }
    std::printf("%d algorithms x %zu engines: carriers, modulation paths and feedback %s\n", count, engines.size(),
                failures ? "DIFFER" : "match the graphs");
    return failures;
}

int topologyTest() {
    const int classic = checkTopology(dx7Algorithms, 32, 6, { 0, 1, 2 });
    const int extended = checkTopology(extAlgorithms, 88, 8, { 3 });
    return classic + extended == 0 ? 0 : 1;
}

// ---- the Extended profile against its measured tables ----

// One note of an Extended sound: operator 1 alone (algorithm 1, every other operator silent,
// instant envelope, release time 0) unless params say otherwise. Mono output at 48 kHz.
std::vector<float> renderExtended(const std::vector<std::pair<uint32_t, double>> &params, int key, double velocity,
                                  double hold, double seconds) {
    Host h(entry);
    const double rate = 48000;
    h.activate(rate, 512);
    EventList in;
    in.events.reserve(512);
    in.param(0, 5, 1);
    in.param(0, 40, 1);
    for (int n = 1; n <= 8; n++) {
        in.param(0, 1000 * n + 51, n == 1 ? 99 : 0);
        in.param(0, 1000 * n + 48, 0);
    }
    for (const auto &p : params)
        in.param(0, p.first, p.second);
    h.process(512, in);
    in.clear();
    const size_t holdFrames = (size_t)(hold * rate), total = (size_t)(seconds * rate);
    std::vector<float> out;
    in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, (int16_t)key, velocity);
    for (size_t done = 0; done < total; done += 512) {
        if (holdFrames >= done && holdFrames < done + 512)
            in.note(CLAP_EVENT_NOTE_OFF, (uint32_t)(holdFrames - done), -1, 0, (int16_t)key, 0);
        h.process(512, in);
        in.clear();
        out.insert(out.end(), h.left.begin(), h.left.begin() + 512);
    }
    return out;
}

// Level in dB per window of `window` samples (peak magnitude), and the window centres in s.
std::vector<double> peakDb(const std::vector<float> &x, size_t window) {
    std::vector<double> db;
    for (size_t i = 0; i + window <= x.size(); i += window) {
        float m = 0;
        for (size_t j = i; j < i + window; j++)
            m = std::max(m, std::fabs(x[j]));
        db.push_back(20 * std::log10(std::max(m, 1e-9f)));
    }
    return db;
}

// Frequency in Hz from upward zero crossings (interpolated) between from and to (samples).
double zeroCrossingHz(const std::vector<float> &x, size_t from, size_t to, double rate = 48000) {
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = from + 1; i < to && i < x.size(); i++)
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = (double)(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0)
                first = t;
            else
                count++;
            last = t;
        }
    return count > 0 ? count * rate / (last - first) : 0;
}

int extendedTest() {
    int failures = 0;
    auto expect = [&](bool ok, const std::string &what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            failures++;
        }
    };
    const double rate = 48000;
    const double f69 = 440.0;
    // -- envelope: decay 1 slopes (ms per dB), fitted between 3 and 35 dB below the peak
    // (from time 20 up: faster decays cross 32 dB within four 64-sample blocks, too few points
    // for this fit; they are checked against the reference renders instead)
    for (int t : { 20, 30, 40, 50, 60, 70, 80 }) {
        const double ms = extEgMsPerDb(t);
        const double seconds = std::min(6.0, 0.05 + 40 * ms / 1000);
        // note 105 (3520 Hz): windows of 48 samples hold three or more peaks
        const auto x = renderExtended({ { 1046, t }, { 1042, 0 }, { 1043, 0 } }, 105, 1.0, seconds, seconds);
        const size_t w = 48;
        const auto db = peakDb(x, w);
        const size_t peak = (size_t)(std::max_element(db.begin(), db.end()) - db.begin());
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int n = 0;
        for (size_t i = peak; i < db.size(); i++) {
            const double rel = db[peak] - db[i];
            if (rel < 3)
                continue;
            if (rel > 35)
                break;
            const double tt = i * w / rate;
            sx += tt; sy += db[i]; sxx += tt * tt; sxy += tt * db[i]; n++;
        }
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx); // dB per second
        const double measured = -1000.0 / slope;
        expect(n > 2 && std::fabs(measured / ms - 1) < 0.02,
               "decay time " + std::to_string(t) + ": " + std::to_string(measured) + " ms/dB, table " + std::to_string(ms));
    }
    // -- hold: the step from the hold level to full level
    for (int t : { 30, 50, 70 }) {
        const double ms = extHoldMs(t);
        const auto x = renderExtended({ { 1049, t }, { 1044, 50 } }, 69, 1.0, ms / 1000 + 0.2, ms / 1000 + 0.2);
        const auto db = peakDb(x, 110);
        const double top = *std::max_element(db.begin(), db.end());
        size_t i = 0;
        while (i < db.size() && db[i] < top - 6)
            i++;
        const double measured = i * 110 / rate * 1000;
        expect(std::fabs(measured - ms) < std::max(0.02 * ms, 110 / rate * 1000),
               "hold time " + std::to_string(t) + ": " + std::to_string(measured) + " ms, table " + std::to_string(ms));
    }
    // -- pitch: ratio, fine, detune, fixed frequency, key follow, pitch velocity (1 cent)
    auto pitchCase = [&](const std::vector<std::pair<uint32_t, double>> &p, int key, double velocity, double expectHz,
                         const std::string &what) {
        const auto x = renderExtended(p, key, velocity, 1.0, 0.6);
        const double hz = zeroCrossingHz(x, 4800, x.size());
        const double cents = 1200 * std::log2(hz / expectHz);
        expect(std::fabs(cents) < 1.0, what + ": " + std::to_string(cents) + " cents");
    };
    auto noteHz = [&](int key) { return f69 * std::pow(2.0, (key - 69) / 12.0); };
    for (int c : { 0, 1, 2, 7, 15, 31 })
        for (int f : { 0, 37, 64, 127 })
            pitchCase({ { 1032, c }, { 1033, f } }, 57, 1.0, noteHz(57) * extRatio(c, f),
                      "coarse " + std::to_string(c) + " fine " + std::to_string(f));
    for (int d : { -15, -7, 1, 15 })
        for (int key : { 36, 60, 96 })
            pitchCase({ { 1034, d } }, key, 1.0, noteHz(key) * std::pow(2.0, extDetuneCents(d, key) / 1200),
                      "detune " + std::to_string(d) + " key " + std::to_string(key));
    for (int c : { 0, 3, 9, 20 })
        for (int f : { 0, 64, 127 }) {
            const double hz = extFixedHz(c, f);
            if (hz > 30 && hz < 12000)
                pitchCase({ { 1031, 1 }, { 1032, c }, { 1033, f } }, 60, 1.0, hz,
                          "fixed coarse " + std::to_string(c) + " fine " + std::to_string(f));
        }
    for (int kf : { 25, 99 })
        for (int key : { 48, 72 })
            pitchCase({ { 1031, 1 }, { 1032, 14 }, { 1035, kf } }, key, 1.0,
                      extFixedHz(14, 0) * std::pow(2.0, extKeyFollowCents(kf, key) / 1200),
                      "key follow " + std::to_string(kf) + " key " + std::to_string(key));
    for (int depth : { -64, 32, 63 })
        for (int pv : { -7, 4, 7 })
            for (int vel : { 1, 64, 127 })
                pitchCase({ { 51, depth }, { 1036, pv } }, 60, vel / 127.0,
                          noteHz(60) * std::pow(2.0, extPitchVelocityCents(depth, pv, vel) / 1200),
                          "pitch velocity " + std::to_string(depth) + "/" + std::to_string(pv) + " velocity " + std::to_string(vel));
    // -- operator pitch EG: each segment straight in cents at its time value's rate; with
    // initial and attack level equal the decay starts at once (1 cent, 4 ms windows)
    for (const auto &pc : std::vector<std::array<int, 4>>{ { 10, 10, 0, 40 }, { -10, -10, 0, 30 }, { 0, 10, 35, 99 } }) {
        const auto x = renderExtended({ { 1037, pc[0] }, { 1038, pc[1] }, { 1039, pc[2] }, { 1040, pc[3] } }, 93, 1.0,
                                      0.3, 0.3);
        const double c0 = extPegCents(pc[0]), c1 = extPegCents(pc[1]);
        const double ra = extPegCentsPerMs(pc[2]), rd = extPegCentsPerMs(pc[3]);
        auto expected = [&](double ms) {
            const double ta = std::fabs(c1 - c0) / ra;
            if (ms < ta)
                return c0 + (c1 > c0 ? 1 : -1) * ra * ms;
            const double d = rd * (ms - ta);
            return std::fabs(c1) <= d ? 0.0 : c1 - (c1 > 0 ? d : -d);
        };
        for (double ms = 4; ms < 60; ms += 8) {
            // the mean frequency between the window's first and last rising zero crossings,
            // compared with the expected pitch midway between them
            double first = -1, last = -1;
            int cycles = 0;
            for (size_t i = (size_t)(ms * rate / 1000) + 1; i < (size_t)((ms + 4) * rate / 1000); i++)
                if (x[i - 1] < 0 && x[i] >= 0) {
                    const double t = (double)(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
                    cycles += first >= 0;
                    first = first < 0 ? t : first;
                    last = t;
                }
            const double mid = (first + last) / 2 / rate * 1000;
            const double got = 1200 * std::log2(cycles * rate / (last - first) / (f69 * 4));
            expect(cycles > 0 && std::fabs(got - expected(mid)) < 1.0,
                   "pitch EG " + std::to_string(pc[0]) + "/" + std::to_string(pc[1]) + "/" + std::to_string(pc[2]) +
                       "/" + std::to_string(pc[3]) + " at " + std::to_string(mid) + " ms: " + std::to_string(got) +
                       " cents, expected " + std::to_string(expected(mid)));
        }
    }
    // -- common pitch EG (M6): static levels per depth, scaled by velocity, and a decay at its
    // time value's rate whatever the depth, then release after note-off (1 cent)
    for (int depth : { 0, 1, 3 })
        for (int lv : { -50, -37, 12, 36, 50 }) {
            if (depth == 0 && lv < -20)
                continue; // below 30 Hz at key 60
            pitchCase({ { 28, lv }, { 29, lv }, { 30, lv }, { 31, lv }, { 32, lv }, { 37, depth } }, 60, 1.0,
                      noteHz(60) * std::pow(2.0, extPartPegCents(lv, depth) / 1200),
                      "common pitch EG level " + std::to_string(lv) + " depth " + std::to_string(depth));
        }
    for (int sens : { 3, 7 })
        for (int vel : { 1, 64 })
            pitchCase({ { 28, 50 }, { 29, 50 }, { 30, 50 }, { 31, 50 }, { 32, 50 }, { 37, 2 }, { 38, sens } }, 60,
                      vel / 127.0,
                      noteHz(60) * std::pow(2.0, extPartPegCents(50, 2) * extPartPegVelocityScale(sens, vel) / 1200),
                      "common pitch EG velocity " + std::to_string(sens) + " velocity " + std::to_string(vel));
    expect(std::fabs(1 / extPartPegTimeKeyFollowScale(7, 36) - 8.0) < 1e-12 &&
               std::fabs(extPartPegVelocityScale(3, 64) - 102 / 128.0) < 1e-12 &&
               extPartPegCentsPerMs(0) == 0 && std::fabs(extPartPegCentsPerMs(99) - 0.05619077) < 1e-9,
           "common pitch EG tables: time key follow, velocity, rates");
    pitchCase({ { 28, 50 }, { 29, 50 }, { 30, 50 }, { 31, 50 }, { 32, 50 }, { 37, 2 }, { 1031, 1 }, { 1032, 16 } }, 60, 1.0,
              extFixedHz(16, 0), "a fixed-frequency operator does not follow the common pitch EG");
    for (const auto &pc : std::vector<std::array<int, 4>>{ { 1, 30, 0, 0 }, { 2, 50, 0, 0 }, { 1, 70, 40, 0 }, { 2, 80, 0, 5 } }) {
        // +50 to 0 by decay 1 at pc[1]; with pc[2], released at 0.1 s towards +50 at that time;
        // time key follow pc[3]
        const int depth = pc[0];
        std::vector<std::pair<uint32_t, double>> p = { { 28, 50 }, { 29, 50 }, { 30, 0 }, { 31, 0 }, { 32, pc[2] ? 50 : 0 },
                                                       { 34, pc[1] }, { 36, pc[2] }, { 37, depth }, { 39, pc[3] },
                                                       { 1048, 99 } }; // (sounding after note-off)
        const double hold = pc[2] ? 0.1 : 1.0;
        const auto x = renderExtended(p, 81, 1.0, hold, 0.3);
        const double key = extPartPegTimeKeyFollowScale(pc[3], 81);
        const double top = extPartPegCents(50, depth), rd = extPartPegCentsPerMs(pc[1]) / key;
        const double rr = pc[2] ? extPartPegCentsPerMs(pc[2]) / key : 0;
        auto expected = [&](double ms) {
            double c = std::max(0.0, top - rd * std::min(ms, hold * 1000));
            if (ms > hold * 1000)
                c = std::min(top, c + rr * (ms - hold * 1000));
            return c;
        };
        for (double ms = 4; ms < 280; ms += 12) {
            double first = -1, last = -1;
            int cycles = 0;
            for (size_t i = (size_t)(ms * rate / 1000) + 1; i < (size_t)((ms + 4) * rate / 1000); i++)
                if (x[i - 1] < 0 && x[i] >= 0) {
                    const double t = (double)(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
                    cycles += first >= 0;
                    first = first < 0 ? t : first;
                    last = t;
                }
            const double mid = (first + last) / 2 / rate * 1000;
            if (std::fabs(mid - hold * 1000) < 4)
                continue; // (the note-off lands within a 64-sample block)
            const double got = 1200 * std::log2(cycles * rate / (last - first) / noteHz(81));
            expect(cycles > 0 && std::fabs(got - expected(mid)) < 1.5,
                   "common pitch EG " + std::to_string(pc[0]) + "/" + std::to_string(pc[1]) + "/" + std::to_string(pc[2]) +
                       "/" + std::to_string(pc[3]) +
                       " at " + std::to_string(mid) + " ms: " + std::to_string(got) + " cents, expected " +
                       std::to_string(expected(mid)));
        }
    }
    // -- operator waves (M5): harmonic levels of waves 1-4 against their table data, relative
    // to a sine at the same level (0.5 dB), from a Hann-windowed DFT at each harmonic
    {
        auto harmonic = [&](const std::vector<float> &x, double hz) {
            const size_t a = 9600, n = 38400; // 0.2 s in, 0.8 s long
            double re = 0, im = 0;
            for (size_t i = 0; i < n; i++) {
                const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n);
                re += w * x[a + i] * std::cos(2 * M_PI * hz * i / rate);
                im += w * x[a + i] * std::sin(2 * M_PI * hz * i / rate);
            }
            return std::hypot(re, im);
        };
        const double f0 = noteHz(48);
        const double sine = harmonic(renderExtended({}, 48, 1.0, 1.1, 1.1), f0);
        for (int w = 1; w <= 4; w++) {
            const int set = (w - 1) * 8 + 3;
            const auto x = renderExtended({ { 1060, w }, { 1061, 3 } }, 48, 1.0, 1.1, 1.1);
            for (int h = 1; h <= 8 && h <= kWaveHarmonicCount[set]; h++) {
                const double want = kWaveHarmonicAmp[kWaveHarmonicOffset[set] + h - 1];
                if (want < 1e-3)
                    continue;
                const double db = 20 * std::log10(harmonic(x, f0 * h) / sine / want);
                expect(std::fabs(db) < 0.5, "wave " + std::to_string(w) + " spread 3 harmonic " + std::to_string(h) +
                                                ": " + std::to_string(db) + " dB from its table");
            }
        }
    }
    // -- where the waves sit in their cycle (it shows when they modulate): waves 1 and 2 run from 0
    // and start at their peak, waves 3 and 4 peak a quarter cycle in, formant 2's window peaks
    // half a cycle in
    {
        extwave::init();
        auto peakAt = [](const int32_t *t) { return (int)(std::max_element(t, t + extwave::kSize) - t); };
        const int slack = extwave::kSize / 256;
        for (int w = 1; w <= 4; w++)
            for (int s = 0; s < 8; s++) {
                const int32_t *t = extwave::table(w, s, 512);
                const int at = peakAt(t), want = w <= 2 ? 0 : extwave::kSize / 4;
                const int off = std::min(std::abs(at - want), extwave::kSize - std::abs(at - want));
                const double lo = *std::min_element(t, t + extwave::kSize) / double(1 << 24);
                expect(off <= slack && (w > 2 || std::fabs(lo) < 0.01),
                       "wave " + std::to_string(w) + " spread " + std::to_string(s) + ": peak at " +
                           std::to_string(at) + ", minimum " + std::to_string(lo));
            }
        for (int s = 1; s < 8; s++) {
            const int at = peakAt(extwave::window(6, s, 511));
            expect(std::abs(at - extwave::kSize / 2) <= slack,
                   "formant 2 spread " + std::to_string(s) + ": window peak at " + std::to_string(at));
        }
    }
    // -- LFO rates (1 %): the LFO alone, period from the wraps of its rising saw; Extended
    // sounds advance it once per sample (every speed up to 1357 Hz)
    for (int range = 0; range < 2; range++)
        for (int speed = 0; speed <= (range ? 415 : 99); speed += range ? 5 : 1) {
            ExtendedVoice v = oscairn_fm::extDefaults();
            v.v[ext::LfoWave] = 2; // saw up
            v.v[ext::LfoSpeedRange] = (int16_t)range;
            v.v[range ? ext::LfoExtSpeed : ext::LfoSpeed] = (int16_t)speed;
            Lfo lfo;
            lfo.init(rate);
            lfo.reset(v);
            expect(lfo.perSample(), "the Extended LFO runs per sample");
            const double hz = range ? extLfoExtendedHz(speed) : extLfoHz(speed);
            const double blockRate = rate; // (ticks: samples)
            const long blocks = (long)std::min(2e8, blockRate * 20 / hz) + 2;
            long first = -1, last = -1, wraps = 0;
            int32_t prev = lfo.getsample();
            for (long b = 1; b < blocks; b++) {
                const int32_t y = lfo.getsample();
                if (y < prev) {
                    if (first < 0)
                        first = b;
                    else
                        wraps++;
                    last = b;
                }
                prev = y;
            }
            // within one sample of the true wrap: compare with the rate over many cycles
            const double measured = wraps > 0 ? wraps * blockRate / (double)(last - first) : 0;
            expect(std::fabs(measured / hz - 1) < 0.01, std::string(range ? "extended" : "normal") + " LFO speed " +
                                                            std::to_string(speed) + ": " + std::to_string(measured) +
                                                            " Hz, table " + std::to_string(hz));
        }
    // -- the filter (milestone M6): each type's steady-state gain against the model its
    // fitted structure gives (Chamberlin sections and a one-pole, docs/oscairn-plan.md
    // section 11), within 0.1 dB where the model is above -60 dB
    {
        using C = std::complex<double>;
        const double fs = 44100;
        auto svf = [&](double f, double fc, double q, int out, double Fover = 0, double dover = -1) {
            float F, d;
            extFilterCoefficients(fc, q, fs, F, d);
            const double g = Fover > 0 ? Fover : F, dd = dover >= 0 ? dover : d;
            const C zi = std::polar(1.0, -2 * M_PI * f / fs), a = 1.0 - zi;
            const C hp = 1.0 / (1.0 + g * g * zi / (a * a) + dd * zi * g / a), bp = g * hp / a, lp = g * zi * bp / a;
            return out == 0 ? lp : out == 1 ? bp : out == 2 ? hp : lp + hp;
        };
        auto onePole = [&](double f, double fc) {
            float F, d;
            extFilterCoefficients(fc, 0.5, fs, F, d);
            const C zi = std::polar(1.0, -2 * M_PI * f / fs);
            return (double)F / (1.0 - (1.0 - F) * zi);
        };
        auto measure = [&](const ExtFilter::Setting &st, double f) {
            ExtFilter flt;
            flt.reset();
            flt.set(st, fs, true);
            const int n = 64, warm = (int)(fs * 0.5), len = (int)(fs * 0.25);
            double c = 0, sn = 0;
            float buf[64];
            for (int i = 0; i < warm + len; i += n) {
                for (int k = 0; k < n; k++)
                    buf[k] = (float)(0.25 * std::sin(2 * M_PI * f * (i + k) / fs));
                flt.set(st, fs, false);
                flt.process(buf, n);
                if (i >= warm)
                    for (int k = 0; k < n; k++) {
                        c += buf[k] * std::cos(2 * M_PI * f * (i + k) / fs);
                        sn += buf[k] * std::sin(2 * M_PI * f * (i + k) / fs);
                    }
            }
            return 20 * std::log10(2 * std::hypot(c, sn) / len / 0.25);
        };
        using namespace extfilter;
        const double fc = extFilterCutoffHz(512), q = extFilterQ(32);
        expect(std::fabs(fc - 448.92) < 0.05 && std::fabs(extFilterCutoffHz(1023) - 7350.2) < 0.1 &&
                   std::fabs(extFilterCutoffHz(0) - 14.33) < 0.01,
               "cutoff table: 512 -> 448.9 Hz, 1023 -> 7350.2 Hz, 0 -> 14.3 Hz");
        expect(std::fabs(q - 2.0) < 1e-9, "resonance 32 is Q 2");
        const double hpf = 200, ratio = std::exp2(24 / 16.0), wideLow = 150;
        struct Case { int type; std::function<C(double)> model; };
        const float F = 2 * (float)std::sin(M_PI * fc / fs);
        const std::vector<Case> cases = {
            { Lpf24D, [&](double f) { return svf(f, fc, q, 0) * svf(f, fc, q, 0); } },
            { Lpf24A, [&](double f) { return svf(f, fc, 1, 0) * svf(f, fc, q, 0); } },
            { Lpf18, [&](double f) { return svf(f, fc, q, 0) * onePole(f, fc); } },
            { Lpf18S, [&](double f) { return onePole(f, fc) * svf(f, fc, q, 0, F * extFilterLpf18sRatio(512), 1 / q); } },
            { Lpf12Hpf12, [&](double f) { return svf(f, fc, q, 0) * svf(f, hpf, 1, 2); } },
            { Lpf6Hpf12, [&](double f) { return onePole(f, fc) * svf(f, hpf, 1, 2); } },
            { Hpf24D, [&](double f) { return svf(f, fc, q, 2) * svf(f, fc, q, 2); } },
            { Hpf12, [&](double f) { return svf(f, fc, q, 2); } },
            { Bpf12D, [&](double f) { return svf(f, fc, q, 1) * svf(f, fc, q, 1); } },
            { BpfWide, [&](double f) { return svf(f, wideLow, 1, 2) * svf(f, fc, 1, 0); } },
            { Bpf6, [&](double f) { return svf(f, fc, q, 1); } },
            { Bef12, [&](double f) { return svf(f, fc, q, 3) * svf(f, fc, q, 3); } },
            { Bef6, [&](double f) { return svf(f, fc, q, 3); } },
            { DualLpf, [&](double f) { return svf(f, fc, q, 0) + svf(f, fc, q, 0, F * ratio, 1 / q); } },
            { DualHpf, [&](double f) { return svf(f, fc, q, 2) + svf(f, fc, q, 2, F * ratio, 1 / q); } },
            { DualBpf, [&](double f) { return svf(f, fc, q, 1) + svf(f, fc, q, 1, F * ratio, 1 / q); } },
            { DualBef, [&](double f) { return svf(f, fc, q, 3) * svf(f, fc, q, 3, F * ratio, 1 / q); } },
            { Lpf12Bpf6, [&](double f) { return svf(f, fc, q, 0) + svf(f, fc, q, 1, 0, 1 / (1.13 * q)); } },
        };
        for (const Case &k : cases) {
            ExtFilter::Setting st;
            st.type = k.type;
            st.fc = fc;
            st.q = q;
            st.ratio2 = k.type == Lpf18S ? extFilterLpf18sRatio(512) : ratio;
            st.fcLow = wideLow;
            st.hpf = hpf;
            double worst = 0, at = 0;
            for (double f : { 60.0, 150.0, 300.0, 420.0, 449.0, 480.0, 700.0, 1200.0, 2500.0, 6000.0 }) {
                const double want = 20 * std::log10(std::abs(k.model(f)) + 1e-12);
                if (want < -60)
                    continue;
                const double err = std::fabs(measure(st, f) - want);
                if (err > worst) {
                    worst = err;
                    at = f;
                }
            }
            expect(worst < 0.1, "filter type " + std::to_string(k.type) + " is its model within 0.1 dB (worst " +
                                    std::to_string(worst) + " dB at " + std::to_string(at) + " Hz)");
        }
        // the limits: no cutoff above 7350.2 Hz, Q at least 1 there, and the gain table
        float Fm, dm;
        extFilterCoefficients(20000, 0.5, fs, Fm, dm);
        expect(std::fabs(Fm - 1.0f) < 1e-3f && std::fabs(dm - 1.0f) < 0.01f, "cutoff stops at F 1 with Q 1");
        // the damping limit at and between its table's points (Q 0.756 measured at cutoff 880)
        expect(std::fabs(1 / extFilterMaxDamping(extFilterCutoffHz(880)) - 0.756) < 0.003 &&
                   std::fabs(1 / extFilterMaxDamping(extFilterCutoffHz(881)) - 0.761) < 0.003,
               "the filter's minimum Q at cutoffs 880 and 881");
        expect(std::fabs(20 * std::log10(extFilterGain(192)) + 23.625) < 1e-6 && extFilterGain(0) == 0 &&
                   extFilterGain(255) == 1.0,
               "filter gain: 0.375 dB per step, 0 silent");
        // the filter EG: each segment takes its time value's duration whatever its distance
        for (int t : { 48, 64, 80 }) {
            for (int lv : { 8, 16, 32 }) {
                ExtFilterEnv env;
                const int levels[5] = { 0, lv, 0, 0, 0 }, times[5] = { 0, 0, t, 0, 0 };
                env.init(levels, times, 1.0, fs);
                const double want = extFegSegmentMs(t) * fs / 1000;
                long n = 0;
                double c = env.getblock(64);
                for (n = 64; c > 0 && n < (long)(fs * 30); n += 64)
                    c = env.getblock(64);
                expect(std::fabs(n - want) <= 128, "FEG decay time " + std::to_string(t) + " from level " +
                                                       std::to_string(lv) + ": " + std::to_string(n) +
                                                       " samples, table " + std::to_string(want));
            }
        }
        // through the plugin: HPF12 at cutoff 512 on a sine at note 36 (65.4 Hz) at 48 kHz, its
        // gain against the same sound through Thru
        {
            auto level = [&](const std::vector<std::pair<uint32_t, double>> &p) {
                const auto x = renderExtended(p, 36, 1.0, 0.5, 0.5);
                double e = 0;
                for (size_t i = 12000; i < 24000; i++)
                    e += (double)x[i] * x[i];
                return 10 * std::log10(e);
            };
            const double thru = level({ { 1048, 0 } });
            const double hp = level({ { 1048, 0 }, { 60, Hpf12 }, { 61, 512 }, { 63, 0 }, { 85, 0 } });
            float F0, d0;
            extFilterCoefficients(extFilterCutoffHz(512), 0.5, 48000, F0, d0);
            const double f = 440 * std::exp2((36 - 69) / 12.0);
            const C zi = std::polar(1.0, -2 * M_PI * f / 48000), a = 1.0 - zi;
            const double want = 20 * std::log10(std::abs(1.0 / (1.0 + (double)F0 * (double)F0 * zi / (a * a) + (double)d0 * zi * (double)F0 / a)));
            expect(std::fabs(hp - thru - want) < 0.2, "HPF12 at cutoff 512 through the plugin: " +
                                                          std::to_string(hp - thru) + " dB, model " + std::to_string(want));
        }
        // key follow: +100 % one octave above note 60 doubles the cutoff, +31.25 % adds 31.25 %
        expect(std::fabs(extFilterKeyFollowRatio(32, 72) - 2.0) < 1e-12 &&
                   std::fabs(extFilterKeyFollowRatio(10, 72) - 1.3125) < 1e-12 &&
                   std::fabs(extFilterKeyFollowRatio(63, 72) - 4.0) < 1e-12,
               "cutoff key follow ratios");
    }
    // -- the two init sounds: Classic is the default sound; Extended plays (operator 8 at
    // level 85 in algorithm 69)
    for (const char *file : { "Classic Init.octet", "Extended Init.octet" }) {
        std::ifstream f(std::string(OCTET_ASSETS) + "/" + file);
        std::stringstream text;
        text << f.rdbuf();
        Host h(entry);
        const bool loaded = h.loadState(text.str());
        double profile = -1;
        h.params->get_value(h.plugin, 5, &profile);
        const bool extended = std::strstr(file, "Extended") != nullptr;
        expect(loaded && profile == (extended ? 1 : 0), std::string(file) + " loads with its profile");
        h.activate(rate, 512);
        EventList in;
        h.process(512, in);
        in.clear();
        in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, 60, 0.8);
        h.process(512, in);
        in.clear();
        h.process(512, in);
        expect(audible(h.left), std::string(file) + " is silent");
    }
    // -- profile conversion
    {
        std::vector<double> v;
        for (const auto &p : oscairn_fm::params())
            v.push_back(p.def);
        auto at = [&](uint32_t id) -> double & { return v[(size_t)oscairn_fm::indexOf(id)]; };
        at(1) = 5;
        at(1003) = 70; // operator 1, rate 3
        oscairn_fm::convertToExtended(v);
        expect(at(5) == 1 && at(40) == 13, "DX7 algorithm 5 converts to Extended algorithm 13");
        expect(at(8051) == 99 && at(1051) == 0 && at(2051) == 0, "DX7 operator 1 plays as Extended operator 8");
        expect(oscairn_fm::convertToClassic(v) && at(5) == 0 && at(1) == 5 && at(1017) == 99,
               "a converted sound converts back to Classic");
        expect(std::fabs(at(1003) - 70) <= 2, "rates survive the round trip within 2 (" + std::to_string(at(1003)) + ")");
        oscairn_fm::convertToExtended(v);
        at(8049) = 10; // a hold segment: not in Classic
        std::vector<double> before = v;
        expect(!oscairn_fm::convertToClassic(v) && v == before, "a sound outside the Classic subset stays Extended");
    }
    std::printf("Extended profile: envelope, pitch and LFO rates against the tables, init sound, conversion %s\n",
                failures ? "FAILED" : "pass");
    return failures == 0 ? 0 : 1;
}

// ---- renders for comparing with reference recordings ----

// render <out.wav> <rate> <note> <velocity 1-127> <hold s> <tail s> [id=value ...]: one note
// with the given parameter values, as a mono 32-bit float WAV (the note starts at frame 0).
int renderToWav(const std::vector<char *> &args) {
    if (args.size() < 8)
        return 2;
    const char *path = args[2];
    const double rate = std::atof(args[3]);
    const int key = std::atoi(args[4]);
    const double velocity = std::atof(args[5]) / 127.0;
    const size_t holdFrames = (size_t)std::llround(std::atof(args[6]) * rate);
    const size_t total = holdFrames + (size_t)std::llround(std::atof(args[7]) * rate);
    Host h(entry);
    h.activate(rate, 512);
    EventList in;
    in.events.reserve(1024);
    for (size_t i = 8; i < args.size(); i++) {
        const char *eq = std::strchr(args[i], '=');
        if (eq == nullptr)
            return 2;
        in.param(0, (clap_id)std::atol(args[i]), std::atof(eq + 1));
    }
    h.process(512, in);
    in.clear();
    std::vector<float> out;
    out.reserve(total + 512);
    in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, (int16_t)key, velocity);
    for (size_t done = 0; done < total; done += 512) {
        if (holdFrames >= done && holdFrames < done + 512)
            in.note(CLAP_EVENT_NOTE_OFF, (uint32_t)(holdFrames - done), -1, 0, (int16_t)key, 0);
        h.process(512, in);
        in.clear();
        out.insert(out.end(), h.left.begin(), h.left.begin() + 512);
    }
    out.resize(total);
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char *)&v, 2); };
    const uint32_t bytes = (uint32_t)(out.size() * 4);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(3); u16(1);
    u32((uint32_t)rate); u32((uint32_t)rate * 4); u16(4); u16(32); f.write("data", 4); u32(bytes);
    f.write((const char *)out.data(), bytes);
    return f ? 0 : 1;
}

// ---- sound contexts ----

struct Instrument {
    Host host;
    const oscairn_plugin_sound_contexts_t *ctx;
    EventList in;
    std::vector<float> out;
    explicit Instrument(bool activate = true) : host(entry) {
        ctx = static_cast<const oscairn_plugin_sound_contexts_t *>(host.plugin->get_extension(host.plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        check(ctx != nullptr, "sound contexts extension");
        in.events.reserve(64);
        if (activate)
            host.activate(48000, 512);
    }
    uint32_t prepare(const char *name, std::vector<oscairn_sound_parameter_t> p, const char *patch = nullptr) {
        char message[256] = {};
        return ctx->prepare(host.plugin, name, p.data(), (uint32_t)p.size(), patch, nullptr, message, sizeof message);
    }
    bool select(uint32_t id, int16_t channel = -1) { return ctx->select(host.plugin, id, channel); }
    bool discard(uint32_t id) { return ctx->discard(host.plugin, id); }
    oscairn_sound_context_info_t info(uint32_t id) {
        oscairn_sound_context_info_t i {};
        for (uint32_t k = 0; k < ctx->capacity(host.plugin); k++)
            if (ctx->get(host.plugin, k, &i) && i.id == id)
                return i;
        fail("missing context " + std::to_string(id));
        return i;
    }
    // renders one block with the queued events, keeping the left channel
    const std::vector<float> &run(uint32_t frames = 512) {
        host.output.clear();
        host.process(frames, in);
        in.clear();
        out.assign(host.left.begin(), host.left.begin() + frames);
        return out;
    }
    void tail() { for (int i = 0; i < 200; i++) run(); }
};

// two clearly different sounds: the init voice, and operator 1 an octave up with a slower attack
const std::vector<oscairn_sound_parameter_t> soundA = {};
const std::vector<oscairn_sound_parameter_t> soundB = { { 1019, 2 }, { 1001, 60 }, { 1017, 90 } };

void same(const std::vector<float> &x, const std::vector<float> &a, const std::vector<float> &b, const char *what) {
    for (size_t i = 0; i < x.size(); i++)
        check(x[i] == a[i] + b[i], what);
}

int contextsTest() {
    const std::vector<float> silence(512, 0.0f);
    {
        Instrument mixed, a, b;
        const uint32_t x = mixed.prepare("A", soundA), y = mixed.prepare("B", soundB);
        check(x != CLAP_INVALID_ID && y != CLAP_INVALID_ID && x != y && x != 0, "prepare two sounds");
        check(mixed.select(x) && a.select(a.prepare("A", soundA)) && b.select(b.prepare("B", soundB)), "select");
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 101, 0, 60);
        a.in.note(CLAP_EVENT_NOTE_ON, 0, 101, 0, 60);
        same(mixed.run(), a.run(), b.run(), "the first sound plays as a standalone instance");
        check(mixed.select(y), "switch while a note is held");
        for (int k = 0; k < 4; k++)
            same(mixed.run(), a.run(), b.run(), "the held note keeps its sound after the switch");
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 102, 0, 60);
        b.in.note(CLAP_EVENT_NOTE_ON, 0, 102, 0, 60);
        same(mixed.run(37), a.run(37), b.run(37), "old and new sounds play together");
        same(mixed.run(), a.run(), b.run(), "old and new sounds play together");
        check(audible(a.out) && audible(b.out) && a.out != b.out, "both sounds are heard and differ");
        mixed.in.param(0, 1017, 70);
        b.in.param(0, 1017, 70);
        same(mixed.run(), a.run(), b.run(), "a parameter edit changes the selected sound, the outgoing one stays");
        check(!mixed.discard(x), "a sounding context cannot be discarded");
        mixed.in.note(CLAP_EVENT_NOTE_OFF, 13, 101, 0, 60);
        a.in.note(CLAP_EVENT_NOTE_OFF, 13, 101, 0, 60);
        same(mixed.run(), a.run(), b.run(), "note ID 101 releases the old sound at its frame");
        mixed.tail();
        check(mixed.info(x).active_voices == 0 && mixed.info(y).active_voices == 1, "the old note ends, the new one plays on");
        check(mixed.info(x).idle && !mixed.info(y).idle, "idle reports");
        check(mixed.discard(x), "a silent unselected context is discarded");
        const uint32_t again = mixed.prepare("A again", soundA);
        check(again != CLAP_INVALID_ID && again != x, "IDs are never reused");
        check(!mixed.select(x), "a stale ID is refused");
    }
    {
        Instrument fifo;
        const uint32_t x = fifo.prepare("A", soundA), y = fifo.prepare("B", soundB);
        const uint8_t on[] = { 0x90, 60, 127 }, off[] = { 0x80, 60, 0 }, down[] = { 0xB0, 64, 127 }, up[] = { 0xB0, 64, 0 };
        fifo.select(x);
        fifo.in.midi(0, on, 3);
        fifo.run();
        fifo.select(y);
        fifo.in.midi(0, on, 3);
        fifo.run();
        fifo.in.midi(0, off, 3);
        fifo.tail();
        check(fifo.info(x).active_voices == 0 && fifo.info(y).active_voices == 1, "a raw MIDI repeated key releases the older sound first");
        fifo.in.midi(0, down, 3);
        fifo.in.midi(1, off, 3);
        fifo.run();
        fifo.select(x);
        fifo.tail();
        check(fifo.info(y).active_voices == 1 && !fifo.discard(y), "the pedal holds the outgoing sound");
        fifo.in.midi(0, up, 3);
        fifo.tail();
        check(fifo.info(y).active_voices == 0, "pedal up reaches the outgoing sound");
        check(fifo.select(y, 1), "select for one channel");
        fifo.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 62);
        fifo.in.note(CLAP_EVENT_NOTE_ON, 0, 2, 1, 65);
        fifo.run();
        check(fifo.info(x).active_voices == 1 && fifo.info(y).active_voices == 1, "channels play their own sounds");
        check(fifo.info(x).selected_channels == 0xfffd && fifo.info(y).selected_channels == 0x2, "selected channels");
        fifo.in.note(CLAP_EVENT_NOTE_OFF, 0, -1, -1, -1);
        fifo.tail();
        check(fifo.info(x).active_voices == 0 && fifo.info(y).active_voices == 0, "a wildcard release reaches every sound");
    }
    {
        // channel parts equal independently rendered instances
        Instrument parts, a, b;
        parts.select(parts.prepare("A", soundA), 0);
        parts.select(parts.prepare("B", soundB), 1);
        a.select(a.prepare("A", soundA));
        b.select(b.prepare("B", soundB));
        parts.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 60);
        parts.in.note(CLAP_EVENT_NOTE_ON, 0, 2, 1, 67);
        a.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 60);
        b.in.note(CLAP_EVENT_NOTE_ON, 0, 2, 1, 67);
        for (int k = 0; k < 20; k++)
            same(parts.run(), a.run(), b.run(), "channel parts equal independent instances");
        check(audible(a.out) && audible(b.out), "both parts are heard");
    }
    {
        Instrument bounded;
        std::vector<uint32_t> ids;
        for (int k = 0; k < 7; k++)
            ids.push_back(bounded.prepare("Ready", {}));
        for (auto id : ids)
            check(id != CLAP_INVALID_ID, "seven sounds besides the default");
        check(bounded.prepare("Too many", {}) == CLAP_INVALID_ID, "the context budget is enforced");
        for (int k = 0; k < 32; k++)
            check(bounded.select(ids[0]), "selection accepted");
        check(!bounded.select(ids[0]) && !bounded.discard(ids[0]), "a full queue refuses, and queued selections pin");
        bounded.run();
        check(bounded.info(ids[0]).selected_channels == 0xffff, "the queued selection applied");
        bounded.select(0);
        bounded.run();
        bounded.run();
        check(bounded.discard(ids[0]), "the queue releases its pins");
        check(!bounded.discard(0), "the default sound cannot be discarded");
    }
    {
        // refused preparations
        Instrument v;
        check(v.prepare("x", { { 999, 1 } }) == CLAP_INVALID_ID, "unknown parameter refused");
        check(v.prepare("x", { { 1, 33 } }) == CLAP_INVALID_ID, "out of range refused");
        check(v.prepare("x", { { 2, 1.5 } }) == CLAP_INVALID_ID, "fraction on a stepped parameter refused");
        check(v.prepare("x", { { 2, 1 }, { 2, 2 } }) == CLAP_INVALID_ID, "duplicate refused");
        check(v.prepare("x", {}, "not a patch") == CLAP_INVALID_ID, "bad patch text refused");
        check(v.prepare(std::string(64, 'n').c_str(), {}) == CLAP_INVALID_ID, "long name refused");
        oscairn_asset_ref_t base {};
        char message[64];
        check(v.ctx->prepare(v.host.plugin, "x", nullptr, 0, nullptr, &base, message, sizeof message) == CLAP_INVALID_ID, "assets refused");
        // patch text: an instance's state prepares the same sound
        Instrument source, a, b;
        EventList setB;
        for (auto &p : soundB)
            setB.param(0, p.id, p.value);
        source.host.process(512, setB);
        const std::string text = source.host.saveState();
        a.select(a.prepare("from text", {}, text.c_str()));
        b.select(b.prepare("B", soundB));
        a.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 64);
        b.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 64);
        for (int k = 0; k < 4; k++)
            same(a.run(), b.run(), silence, "patch text prepares the same sound");
        check(audible(a.out), "the patch-text sound is heard");
    }
    {
        // switching renders the same in 37- and 512-frame blocks
        auto timeline = [&](uint32_t block) {
            Instrument t;
            const uint32_t first = t.prepare("A", soundA), next = t.prepare("B", soundB);
            t.select(first);
            std::vector<float> result;
            for (uint32_t pos = 0; pos < 4096;) {
                if (pos == 256)
                    t.select(next);
                const uint32_t n = std::min({ block, 4096 - pos, pos < 256 ? 256 - pos : 4096 - pos });
                for (auto [at, on, id] : { std::tuple{ 64u, true, 1 }, std::tuple{ 300u, true, 2 }, std::tuple{ 700u, false, 1 }, std::tuple{ 900u, false, 2 } })
                    if (at >= pos && at < pos + n)
                        t.in.note(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, at - pos, id, 0, 60);
                const auto &o = t.run(n);
                result.insert(result.end(), o.begin(), o.end());
                pos += n;
            }
            return result;
        };
        const auto t37 = timeline(37);
        check(t37 == timeline(512) && audible(t37), "context switching is the same in 37- and 512-frame blocks");
    }
    {
        // the exclusive snapshot: save, load into a fresh instance, save again
        Instrument a(false);
        const uint32_t x = a.prepare("A", soundA), y = a.prepare("B", soundB);
        a.select(x);
        a.select(y, 3);
        std::string saved;
        clap_ostream_t os { &saved, [](const clap_ostream_t *st, const void *buf, uint64_t size) -> int64_t {
                               static_cast<std::string *>(st->ctx)->append(static_cast<const char *>(buf), size);
                               return (int64_t)size;
                           } };
        check(a.ctx->state->save(a.host.plugin, &os), "snapshot save");
        Instrument b(false);
        struct Reader { const std::string *d; size_t pos; } r { &saved, 0 };
        clap_istream_t is { &r, [](const clap_istream_t *st, void *buf, uint64_t size) -> int64_t {
                               auto *rr = static_cast<Reader *>(st->ctx);
                               const size_t n = std::min<size_t>(size, rr->d->size() - rr->pos);
                               std::memcpy(buf, rr->d->data() + rr->pos, n);
                               rr->pos += n;
                               return (int64_t)n;
                           } };
        check(b.ctx->state->load(b.host.plugin, &is), "snapshot load");
        std::string again;
        clap_ostream_t os2 { &again, os.write };
        check(b.ctx->state->save(b.host.plugin, &os2) && again == saved, "snapshot round trip gives the same bytes");
        std::string badHex = saved;
        const size_t ctxLine = badHex.find("context 1 ");
        check(ctxLine != std::string::npos, "snapshot has context 1");
        badHex.replace(ctxLine + 10, 2, "zz");
        r = { &badHex, 0 };
        check(!b.ctx->state->load(b.host.plugin, &is), "a snapshot name that is not hex is refused");
        std::string bad = saved;
        bad.replace(0, 8, "XXXXXXXX");
        r = { &bad, 0 };
        check(!b.ctx->state->load(b.host.plugin, &is), "a bad snapshot is refused");
    }
    {
        // Controllers per channel. Two notes on MIDI channel 2 first, so MPE (where channel 1
        // is the master for all) is off, as it is for any non-MPE source.
        auto setup = [](Instrument &t) {
            // mod wheel to pitch and amp, an LFO for it to scale, amp mod sensitivity on op 1
            t.in.param(0, 110, 99);
            t.in.param(0, 111, 1);
            t.in.param(0, 112, 1);
            t.in.param(0, 10, 60);
            t.in.param(0, 16, 7);
            t.in.param(0, 1015, 3);
            t.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 1, 60);
            t.in.note(CLAP_EVENT_NOTE_ON, 0, 2, 1, 64);
            t.run();
        };
        auto channelTwo = [&](const uint8_t *message, int size) {
            Instrument t;
            setup(t);
            if (message != nullptr)
                t.in.midi(0, message, size);
            std::vector<float> r;
            for (int k = 0; k < 8; k++) {
                const auto &o = t.run();
                r.insert(r.end(), o.begin(), o.end());
            }
            check(audible(r), "channel 2 is heard");
            return r;
        };
        const auto plain = channelTwo(nullptr, 0);
        const uint8_t wheel1[] = { 0xB0, 1, 127 }, wheel2[] = { 0xB1, 1, 127 };
        const uint8_t bend1[] = { 0xE0, 0, 127 }, bend2[] = { 0xE1, 0, 127 };
        const uint8_t press1[] = { 0xD0, 127 };
        check(channelTwo(wheel1, 3) == plain, "the mod wheel on channel 1 does not change channel 2");
        check(channelTwo(wheel2, 3) != plain, "the mod wheel on channel 2 changes channel 2");
        check(channelTwo(bend1, 3) == plain, "pitch bend on channel 1 does not bend channel 2");
        check(channelTwo(bend2, 3) != plain, "pitch bend on channel 2 bends channel 2");
        check(channelTwo(press1, 2) == plain, "channel pressure on channel 1 does not reach channel 2");

        // Sustain on channel 1 holds channel 1 only.
        Instrument t;
        t.in.note(CLAP_EVENT_NOTE_ON, 0, 10, 0, 60);
        t.in.note(CLAP_EVENT_NOTE_ON, 0, 11, 0, 62);
        t.in.note(CLAP_EVENT_NOTE_ON, 0, 20, 1, 67);
        t.in.note(CLAP_EVENT_NOTE_ON, 0, 21, 1, 69);
        t.run();
        const uint8_t down1[] = { 0xB0, 64, 127 };
        t.in.midi(0, down1, 3);
        t.in.note(CLAP_EVENT_NOTE_OFF, 10, -1, 0, -1);
        t.in.note(CLAP_EVENT_NOTE_OFF, 10, -1, 1, -1);
        std::set<int32_t> ended;
        for (int k = 0; k < 200; k++) {
            t.run();
            for (auto &e : ends(t.host))
                ended.insert(e.id);
        }
        check(ended == std::set<int32_t>{ 20, 21 }, "sustain on channel 1 holds channel 1 and not channel 2");

        // CC 120 on channel 1 stops channel 1 only, with NOTE_END for exactly its voices.
        Instrument mixed, ref;
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 60);
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 2, 0, 64);
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 3, 1, 67);
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 4, 1, 71);
        ref.in.note(CLAP_EVENT_NOTE_ON, 0, 3, 1, 67);
        ref.in.note(CLAP_EVENT_NOTE_ON, 0, 4, 1, 71);
        mixed.run();
        ref.run();
        const uint8_t off1[] = { 0xB0, 120, 0 };
        mixed.in.midi(100, off1, 3);
        mixed.run();
        ref.run();
        auto e = ends(mixed.host);
        std::set<int32_t> stopped;
        for (auto &x : e) {
            stopped.insert(x.id);
            check(x.time == 100 && x.channel == 0, "NOTE_END at the CC 120 frame, on channel 1");
        }
        check(stopped == std::set<int32_t>{ 1, 2 }, "CC 120 on channel 1 ends exactly channel 1's notes");
        // Channel 2 plays on as if channel 1 had never played, up to what the output's DC
        // filter still holds of channel 1 (a small, decaying offset).
        float first = 0, last = 0;
        for (int k = 0; k < 8; k++) {
            const auto m = mixed.run();
            ref.run();
            check(audible(ref.out), "channel 2 is heard");
            float worst = 0;
            for (size_t i = 0; i < m.size(); i++)
                worst = std::max(worst, std::fabs(m[i] - ref.out[i]));
            (k == 0 ? first : last) = worst;
            check(ends(mixed.host).empty(), "no more NOTE_END");
        }
        std::printf("after CC 120: channel 2 differs from a channel-2-only render by %g, then %g\n", first, last);
        check(first < 1e-2f && last < first, "channel 2 plays on as if channel 1 had never played");

        // CC 123 on channel 2 releases channel 2's held notes only.
        const uint8_t notesOff2[] = { 0xB1, 123, 0 };
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 5, 0, 48);
        mixed.in.midi(0, notesOff2, 3);
        std::set<int32_t> released;
        for (int k = 0; k < 200; k++) {
            mixed.run();
            for (auto &x : ends(mixed.host))
                released.insert(x.id);
        }
        check(released == std::set<int32_t>{ 3, 4 }, "CC 123 on channel 2 releases channel 2's notes only");
    }
    {
        // A raw MIDI note-off releases the voice with its channel and key, nothing else.
        auto endedAfter = [](std::vector<std::array<uint8_t, 3>> ons, std::array<uint8_t, 3> off) {
            Instrument t;
            for (auto &m : ons)
                t.in.midi(0, m.data(), 3);
            t.run();
            t.in.midi(0, off.data(), 3);
            std::vector<std::pair<int, int>> ended; // channel, key
            for (int k = 0; k < 200; k++) {
                t.run();
                for (auto &e : ends(t.host))
                    ended.push_back({ e.channel, e.key });
            }
            return ended;
        };
        // MPE detection is on until two notes share a channel: one note on channel 2
        check(endedAfter({ { 0x91, 60, 100 } }, { 0x81, 62, 0 }).empty(), "a note-off for key 62 does not release key 60 (MPE on)");
        const auto both = endedAfter({ { 0x90, 60, 100 }, { 0x91, 60, 100 } }, { 0x81, 60, 0 });
        check(both.size() == 1 && both[0] == std::make_pair(1, 60), "a note-off on channel 2 releases channel 2's key 60 only");
        const auto noMpe = endedAfter({ { 0x91, 60, 100 }, { 0x91, 64, 100 } }, { 0x81, 62, 0 });
        check(noMpe.empty(), "a note-off for key 62 does not release key 60 (MPE off)");
    }
    std::printf("held sounds, parts, note ownership, sustain, capacity, queue, reclamation, patch text, block sizes, snapshots, per-channel controllers, CC 120, CC 123 and note-off matching\n");
    return 0;
}

// ---- state and flush while active ----

std::string stateWith(Host &h, clap_id id, double v) {
    // a copy of h's state with one parameter changed
    std::string st = h.saveState();
    const std::string key = "p " + std::to_string(id) + " ";
    const size_t at = st.find("\n" + key);
    check(at != std::string::npos, "parameter in state");
    const size_t end = st.find('\n', at + 1);
    std::ostringstream line;
    line.imbue(std::locale::classic());
    line << "\n" << key << v;
    st.replace(at, end - at, line.str());
    return st;
}

double valueOf(Host &h, clap_id id) {
    double v;
    check(h.params->get_value(h.plugin, id, &v), "get_value");
    return v;
}

void flushParam(Host &h, clap_id id, double v) {
    EventList in, out;
    in.param(0, id, v);
    out.events.reserve(512);
    const clap_input_events_t input = in.input();
    const clap_output_events_t output = out.output();
    h.params->flush(h.plugin, &input, &output);
}

int stateActiveTest() {
    {
        Instrument t;
        const std::string s1 = stateWith(t.host, 1017, 42), s2 = stateWith(t.host, 1017, 77);
        check(t.host.loadState(s1), "active load");
        check(valueOf(t.host, 1017) == 42 && t.host.saveState() == s1, "get_value and save show an active load at once");
        check(t.host.loadState(s2), "a second active load without processing");
        check(valueOf(t.host, 1017) == 77 && t.host.saveState() == s2, "the second load wins");
        std::string bad = s2;
        bad += "p 1 99\n";
        check(!t.host.loadState(bad), "a bad state is refused");
        check(t.host.saveState() == s2, "a refused load changes nothing");
        std::string badName = s2;
        badName.replace(badName.find("name ") + 5, 2, "zz");
        check(!t.host.loadState(badName) && t.host.saveState() == s2, "a name that is not hex is refused");
        t.run();
        check(t.host.saveState() == s2, "the engine takes the last load");
    }
    {
        // a load right after select(B), before any process: name and values on the same sound
        Instrument t;
        const uint32_t b = t.prepare("B", soundB);
        std::string named = stateWith(t.host, 1017, 33);
        named.replace(named.find("name ") + 5, 20, "4c4f4144454420414141"); // "LOADED AAA"
        check(t.select(b), "select B");
        check(t.host.loadState(named), "load before the selection is processed");
        t.run();
        // the load went to the sound shown when it arrived (the default one), all of it
        check(t.select(0), "select the default sound again");
        t.run();
        check(t.host.saveState() == named, "name and values landed on the same sound");
    }
    {
        // flush while active applies queued selections first (RFC 0005)
        Instrument t;
        const uint32_t b = t.prepare("B", soundB);
        const std::string a0 = t.host.saveState();
        check(t.select(b), "select B");
        flushParam(t.host, 1017, 11);
        t.run();
        check(valueOf(t.host, 1017) == 11, "the flushed edit is on B, the selected sound");
        check(t.select(0), "back to the default sound");
        t.run();
        check(t.host.saveState() == a0, "the default sound did not get the edit");
    }
    {
        // a parameter event for one channel edits the sound selected on that channel only
        Instrument t;
        const uint32_t b = t.prepare("B", soundB);
        check(t.select(b, 1), "B on channel 2");
        const std::string a0 = t.host.saveState();
        clap_event_param_value_t e {};
        e.header = { sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = 1017;
        e.note_id = e.port_index = e.key = -1;
        e.channel = 1;
        e.value = 21;
        t.in.push(e);
        t.run();
        check(t.host.saveState() == a0, "channel 1's sound is unchanged");
        check(t.select(b), "show B");
        t.run();
        check(valueOf(t.host, 1017) == 21, "channel 2's sound has the edit");
    }
    std::printf("active loads visible at once, last load wins, refused loads change nothing, one target sound, flush after selections, per-channel parameter events\n");
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: see the top of oscairn_fm_test.cpp\n");
        return 2;
    }
    std::vector<char *> args(argv, argv + argc);
    for (size_t i = 0; i + 1 < args.size(); i++)
        if (std::strcmp(args[i], "--module") == 0) {
            entry = loadEntry(args[i + 1]);
            args.erase(args.begin() + (long)i, args.begin() + (long)i + 2);
            break;
        }
    check(entry != nullptr, "no plugin: pass --module PATH");
    const std::string cmd = args[1];
    if (cmd == "baseline" && args.size() >= 3) {
        char *a[] = { args[0], (char *)"--check", args[2], args.size() > 3 ? args[3] : nullptr };
        return runBaseline(args.size() > 3 ? 4 : 3, a, renderCase);
    }
    if (cmd == "baseline-write" && args.size() >= 3) {
        char *a[] = { args[0], (char *)"--write", args[2] };
        return runBaseline(3, a, renderCase);
    }
    if (cmd == "two-rates" && args.size() >= 3)
        return twoRates(args[2]);
    if (cmd == "params" && args.size() >= 3)
        return paramsTest(args[2], args.size() > 3 && std::strcmp(args[3], "--write") == 0);
    if (cmd == "notes")
        return notesTest();
    if (cmd == "state")
        return stateTest();
    if (cmd == "no-alloc")
        return noAllocTest();
    if (cmd == "threads")
        return threadsTest();
    if (cmd == "extended")
        return extendedTest();
    if (cmd == "render")
        return renderToWav(args);
    if (cmd == "topology")
        return topologyTest();
    if (cmd == "contexts")
        return contextsTest();
    if (cmd == "state-active")
        return stateActiveTest();
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
