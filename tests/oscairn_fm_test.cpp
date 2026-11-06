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
 *   oscairn-fm-test snapshots                        a snapshot's sounds prepared beside the playing ones
 *   oscairn-fm-test presets                          cartridge voices through CLAP preset discovery
 *   oscairn-fm-test bench [seconds]                  CPU: real-time factor and voices per core (no gate)
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
#include "msfa/ext_part_lfo.h"
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
#include <memory>
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
std::string modulePath; // --module, for the built-in cartridges beside it

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
        for (int k = 0; k < 100; k++) {
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
    auto *snapshots = static_cast<const oscairn_plugin_sound_snapshots_t *>(h.plugin->get_extension(h.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
    check(snapshots != nullptr, "oscairn.sound-snapshots offered");
    std::string snapshot; // one sound, prepared beside while processing
    clap_ostream_t os { &snapshot, [](const clap_ostream_t *st, const void *buf, uint64_t size) -> int64_t {
                           static_cast<std::string *>(st->ctx)->append(static_cast<const char *>(buf), size);
                           return (int64_t)size;
                       } };
    check(ctx->state->save(h.plugin, &os), "snapshot save");
    uint32_t prepared = 0, beside = 0;
    for (int i = 0; i < 300; i++) {
        for (uint32_t k = 1; k < ctx->capacity(h.plugin); k++) {
            oscairn_sound_context_info_t info;
            if (ctx->get(h.plugin, k, &info) && info.idle)
                ctx->discard(h.plugin, info.id);
        }
        const oscairn_sound_parameter_t p[] = { { 1019, (double)(i % 4) } };
        if (i % 10 == 5) {
            struct Reader { const std::string *d; size_t pos; } r { &snapshot, 0 };
            clap_istream_t is { &r, [](const clap_istream_t *st, void *buf, uint64_t size) -> int64_t {
                                   auto *rr = static_cast<Reader *>(st->ctx);
                                   const size_t n = std::min<size_t>(size, rr->d->size() - rr->pos);
                                   std::memcpy(buf, rr->d->data() + rr->pos, n);
                                   rr->pos += n;
                                   return (int64_t)n;
                               } };
            uint32_t selected[16];
            if (snapshots->prepare(h.plugin, &is, selected, nullptr, 0)) {
                beside++;
                ctx->select(h.plugin, selected[0], -1);
            }
            continue;
        }
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
    check(beside > 0, "snapshots prepared beside while processing");
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
    std::printf("main-thread parameter reads, saves, loads and context prepare/select/discard while processing (%u prepared, %u snapshots beside)\n", prepared, beside);
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
// instant envelope, release time 0) unless params say otherwise. Mono output at 48 kHz unless
// rate says otherwise.
std::vector<float> renderExtended(const std::vector<std::pair<uint32_t, double>> &params, int key, double velocity,
                                  double hold, double seconds, std::vector<float> *right = nullptr, double rate = 48000) {
    Host h(entry);
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
        if (right != nullptr)
            right->insert(right->end(), h.right.begin(), h.right.begin() + 512);
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

// An Extended sound (operator 1 a carrier, release 0) playing raw MIDI note events on channel 1
// at 48 kHz: {seconds, key, velocity (0: note-off)}.
std::vector<float> renderMidiNotes(const std::vector<std::pair<uint32_t, double>> &params,
                                   const std::vector<std::array<double, 3>> &notes, double seconds,
                                   double rate = 48000) {
    Host h(entry);
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
    std::vector<float> out;
    const size_t total = (size_t)(seconds * rate);
    for (size_t done = 0; done < total; done += 512) {
        for (const auto &n : notes) {
            const size_t f = (size_t)(n[0] * rate);
            if (f >= done && f < done + 512) {
                const uint8_t b[3] = { (uint8_t)(n[2] > 0 ? 0x90 : 0x80), (uint8_t)n[1], (uint8_t)n[2] };
                in.midi((uint32_t)(f - done), b, 3);
            }
        }
        h.process(512, in);
        in.clear();
        out.insert(out.end(), h.left.begin(), h.left.begin() + 512);
    }
    return out;
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
    // the reference's pitch grid (extNoteSteps): A4 is step 13012 of 2048 an octave, every key
    // rounded up to the 1/1024-octave grid from it, fixed frequencies in 1/128-octave fine steps
    // from A4's step
    auto noteSteps = [](int key) { return 13012 + 2 * (int)std::ceil((key - 69) * 1024 / 12.0 - 1e-9); };
    const double f69 = extStepsHz(13012);
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
    // -- AEG time key follow: a decay of time 35 (rate step 75) at sensitivity 7 moves by
    // 7 x 12 / 4 = 21 steps 36 keys above and below the centre (notes 105 and 30: 4.19 kHz and a
    // fixed 3.5 kHz carrier), to steps 96 and 54 (time 5, step 113, is held at step 127); notes
    // 64-69 do not move; sensitivity 3 at note 105 moves 9 steps (`m15k`)
    for (const auto &kc : std::vector<std::array<double, 4>>{ { 35, 7, 105, 5632.0 / 32768 }, { 35, 7, 30, 5632.0 / 896 },
                                                             { 35, 7, 66, 1 }, { 35, 3, 105, 5632.0 / 12288 } }) {
        const int t = (int)kc[0], key = (int)kc[2];
        const double ms = extEgMsPerDb(t) * kc[3];
        const double seconds = std::min(6.0, 0.05 + 40 * ms / 1000);
        std::vector<std::pair<uint32_t, double>> p = { { 1046, t }, { 1042, 0 }, { 1043, 0 }, { 1050, kc[1] } };
        if (key < 100) // a fixed 3.5 kHz carrier (coarse 19)
            p.insert(p.end(), { { 1031, 1 }, { 1032, 19 } });
        const auto x = renderExtended(p, key, 1.0, seconds, seconds);
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
        const double measured = -1000.0 / ((n * sxy - sx * sy) / (n * sxx - sx * sx));
        expect(n > 2 && std::fabs(measured / ms - 1) < 0.03,
               "time key follow " + std::to_string((int)kc[1]) + " time " + std::to_string(t) + " note " + std::to_string(key) +
                   ": " + std::to_string(measured) + " ms/dB, expected " + std::to_string(ms));
    }
    expect(extTimeKeyFollowSteps(7, 21) == -26 && extTimeKeyFollowSteps(7, 22) == -24 && extTimeKeyFollowSteps(7, 63) == -1 &&
               extTimeKeyFollowSteps(7, 64) == 0 && extTimeKeyFollowSteps(7, 69) == 0 && extTimeKeyFollowSteps(7, 70) == 1 &&
               extTimeKeyFollowSteps(7, 112) == 26 && extTimeKeyFollowSteps(7, 0) == -26 && extTimeKeyFollowSteps(5, 101) == 13 &&
               std::fabs(extTimeKeyFollowScale(21, 5) - 147456.0 / 491520) < 1e-12 &&
               std::fabs(extTimeKeyFollowScale(-21, 80) - 4.5) < 1e-12,
           "time key follow steps by note, and the rate held within steps 0-127");
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
    auto noteHz = [&](int key) { return extStepsHz(noteSteps(key)); };
    auto fixedHz = [&](int coarse, int fine) { return extStepsHz(13012 + 2048 * (coarse - 16) + 16 * fine); };
    {
        // past coarse 21 fine 0 (14.09 kHz): coarse 21 fine 64 at 19.92 kHz, as the reference plays it
        // (`m15f`), at 96 kHz
        const auto x = renderExtended({ { 1031, 1 }, { 1032, 21 }, { 1033, 64 } }, 60, 1.0, 1.0, 0.6, nullptr, 96000);
        const double cents = 1200 * std::log2(zeroCrossingHz(x, 9600, x.size(), 96000) / fixedHz(21, 64));
        expect(std::fabs(cents) < 1 && std::fabs(fixedHz(21, 64) - 19920) < 2,
               "fixed coarse 21 fine 64: " + std::to_string(cents) + " cents from " + std::to_string(fixedHz(21, 64)) + " Hz");
    }
    for (int c : { 0, 1, 2, 7, 15, 31 })
        for (int f : { 0, 37, 64, 127 })
            pitchCase({ { 1032, c }, { 1033, f } }, 57, 1.0, noteHz(57) * extRatio(c, f),
                      "coarse " + std::to_string(c) + " fine " + std::to_string(f));
    for (int d : { -15, -7, 1, 15 })
        for (int key : { 36, 60, 96 })
            pitchCase({ { 1034, d } }, key, 1.0, noteHz(key) * std::pow(2.0, extDetuneCents(d, key) / 1200),
                      "detune " + std::to_string(d) + " key " + std::to_string(key));
    // a fixed-frequency operator takes its own detune (by the key, as a ratio one) but not the part's
    for (int key : { 48, 72 }) {
        pitchCase({ { 1031, 1 }, { 1032, 15 }, { 1034, 15 } }, key, 1.0,
                  fixedHz(15, 0) * std::pow(2.0, extDetuneCents(15, key) / 1200),
                  "fixed detune 15 key " + std::to_string(key));
        pitchCase({ { 1031, 1 }, { 1032, 15 }, { 245, 127 } }, key, 1.0, fixedHz(15, 0),
                  "part detune leaves a fixed operator alone, key " + std::to_string(key));
    }
    for (int c : { 0, 3, 9, 20 })
        for (int f : { 0, 64, 127 }) {
            const double hz = fixedHz(c, f);
            if (hz > 30 && hz < 12000)
                pitchCase({ { 1031, 1 }, { 1032, c }, { 1033, f } }, 60, 1.0, hz,
                          "fixed coarse " + std::to_string(c) + " fine " + std::to_string(f));
        }
    for (int kf : { 25, 99 })
        for (int key : { 48, 72 })
            pitchCase({ { 1031, 1 }, { 1032, 14 }, { 1035, kf } }, key, 1.0,
                      fixedHz(14, 0) * std::pow(2.0, extKeyFollowCents(kf, key) / 1200),
                      "key follow " + std::to_string(kf) + " key " + std::to_string(key));
    for (int depth : { -64, 32, 63 })
        for (int pv : { -7, 4, 7 })
            for (int vel : { 1, 64, 127 })
                pitchCase({ { 51, depth }, { 1036, pv } }, 60, vel / 127.0,
                          noteHz(60) * std::pow(2.0, extPitchVelocityCents(depth, pv, vel) / 1200),
                          "pitch velocity " + std::to_string(depth) + "/" + std::to_string(pv) + " velocity " + std::to_string(vel));
    // the reference's tuning, to a fiftieth of a cent, against its renders (`m13u`, `m13t`, `m14s`:
    // cents from equal temperament at A = 440 Hz): every pitch class of an octave on a ratio-1
    // carrier, fixed frequencies at four fine values, two ratios and two detunes; timed before the
    // note-off
    auto tuning = [&](const std::vector<std::pair<uint32_t, double>> &p, int key, double nominalHz, double want, const std::string &what) {
        const auto x = renderExtended(p, key, 1.0, 1.0, 1.0);
        const double c = 1200 * std::log2(zeroCrossingHz(x, 4800, 46000) / nominalHz);
        expect(std::fabs(c - want) < 0.02, "tuning of " + what + ": " + std::to_string(c) + " cents, want " + std::to_string(want));
    };
    const double measured[12] = { 0.909, 1.671, 1.107, 0.763, 1.627, 1.317, 0.896, 1.380, 1.262, 0.665, 1.548, 1.300 };
    for (int key = 60; key < 72; key++)
        tuning({}, key, 440.0 * std::pow(2.0, (key - 69) / 12.0), measured[key - 60], "key " + std::to_string(key));
    for (const auto &xf : std::vector<std::pair<int, double>>{ { 0, 0.665 }, { 1, 0.857 }, { 37, 0.686 }, { 100, 0.754 } })
        tuning({ { 1031, 1 }, { 1032, 16 }, { 1033, xf.first } }, 60, 440.0 * std::pow(2.0, xf.first / 128.0), xf.second,
               "fixed fine " + std::to_string(xf.first));
    tuning({ { 1032, 5 }, { 1033, 37 } }, 62, 440.0 * std::pow(2.0, -7 / 12.0) * 6.85, 1200 * std::log2(2013.3545 / (440.0 * std::pow(2.0, -7 / 12.0) * 6.85)), "ratio 6.85 at key 62");
    tuning({ { 1032, 0 }, { 1033, 50 } }, 62, 440.0 * std::pow(2.0, -7 / 12.0) * 0.75, 1200 * std::log2(220.3790 / (440.0 * std::pow(2.0, -7 / 12.0) * 0.75)), "ratio 0.75 at key 62");
    tuning({ { 1034, 7 } }, 60, 440.0 * std::pow(2.0, -9 / 12.0), 1200 * std::log2(262.9826 / (440.0 * std::pow(2.0, -9 / 12.0))), "detune 7 at key 60");
    tuning({ { 1034, 14 } }, 60, 440.0 * std::pow(2.0, -9 / 12.0), 1200 * std::log2(265.2958 / (440.0 * std::pow(2.0, -9 / 12.0))), "detune 14 at key 60");
    // the operator's pitch velocity sensitivity moves fixed-frequency operators only, straight in
    // velocity from 64 (+7: 1504 cents at velocity 127)
    for (int pv : { -7, 3, 7 })
        for (int vel : { 1, 40, 64, 127 })
            pitchCase({ { 1031, 1 }, { 1032, 16 }, { 1036, pv } }, 60, vel / 127.0,
                      fixedHz(16, 0) * std::pow(2.0, extFixedPitchVelocityCents(pv, vel) / 1200),
                      "fixed pitch velocity " + std::to_string(pv) + " velocity " + std::to_string(vel));
    expect(std::fabs(extFixedPitchVelocityCents(7, 127) - 1504.05) < 1e-9 && std::fabs(extFixedPitchVelocityCents(-7, 96) + 1504.05 * 32 / 63) < 1e-9,
           "fixed pitch velocity table");
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
              fixedHz(16, 0), "a fixed-frequency operator does not follow the common pitch EG");
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
    // -- pitch key follow (straight in cents from the centre) and random pitch, ratio-mode
    // operators only (1 cent)
    for (int kf : { 0, 40, 80, 112, 127 })
        for (int key : { 50, 84 })
            pitchCase({ { 56, kf } }, key, 1.0, noteHz(key) * std::pow(2.0, extPitchKeyFollowCents(kf, key, 60) / 1200),
                      "pitch key follow " + std::to_string(kf) + " key " + std::to_string(key));
    pitchCase({ { 56, 127 }, { 57, 48 } }, 41, 1.0, noteHz(41) * std::pow(2.0, -700 / 1200.0),
              "pitch key follow +200 % centre 48, key 41");
    pitchCase({ { 56, 0 }, { 55, 127 }, { 1031, 1 }, { 1032, 16 } }, 84, 1.0, fixedHz(16, 0),
              "a fixed-frequency operator follows neither pitch key follow nor random pitch");
    expect(extPitchKeyFollowPercent(64) == 0 && extPitchKeyFollowPercent(96) == 100 &&
               extPitchKeyFollowPercent(0) == -200 && extPitchKeyFollowPercent(127) == 200,
           "pitch key follow percent table ends");
    {
        // random pitch: whole 18.75-cent steps within depth / 4 steps, every step reached
        bool ok = true;
        double lo = 0, hi = 0;
        uint32_t r = 1;
        for (int i = 0; i < 20000; i++) {
            r = r * 1664525u + 1013904223u;
            const double c = extRandomPitchCents(127, r);
            ok = ok && std::fabs(c / 18.75 - std::round(c / 18.75)) < 1e-9;
            lo = std::min(lo, c);
            hi = std::max(hi, c);
        }
        // depth 4 always plays 0, depth 8 at most one step, depth 64 at most 15 (cut towards 0)
        bool small = true;
        for (int i = 0; i < 20000; i++) {
            r = r * 1664525u + 1013904223u;
            small = small && extRandomPitchCents(4, r) == 0 && std::fabs(extRandomPitchCents(8, r)) <= 18.75 &&
                    std::fabs(extRandomPitchCents(64, r)) <= 15 * 18.75;
        }
        expect(ok && small && lo == -31 * 18.75 && hi == 31 * 18.75 && extRandomPitchCents(0, 12345) == 0 &&
                   std::fabs(extRandomPitchCents(16, 0xffffffffu)) <= 75,
               "random pitch steps and range");
        int panLo = 0, panHi = 0;
        for (int i = 0; i < 20000; i++) {
            r = r * 1664525u + 1013904223u;
            panLo = std::min(panLo, extRandomPanPosition(64, r));
            panHi = std::max(panHi, extRandomPanPosition(64, r));
        }
        expect(panLo == -32 && panHi == 32 && extRandomPanPosition(0, 99) == 0, "random pan within depth / 2 positions");
        // the same note twice after activation plays the same random pitch (deterministic), on
        // the grid
        const auto a = renderExtended({ { 55, 127 } }, 60, 1.0, 0.6, 0.6);
        const auto b = renderExtended({ { 55, 127 } }, 60, 1.0, 0.6, 0.6);
        const double cents = 1200 * std::log2(zeroCrossingHz(a, 4800, a.size()) / noteHz(60));
        expect(a == b && std::fabs(cents / 18.75 - std::round(cents / 18.75)) < 0.06 && std::fabs(cents) <= 600.5,
               "random pitch is deterministic and on its grid: " + std::to_string(cents) + " cents");
    }
    // -- key-on delay: the note starts that many samples late (exact, within the 64-sample
    // grid), tempo-synced lengths at the host's tempo (120 without transport), and a note
    // released before its delay ends never sounds
    {
        auto firstSound = [](const std::vector<float> &x) {
            size_t i = 0;
            while (i < x.size() && x[i] == 0)
                i++;
            return i;
        };
        for (int kd : { 1, 7, 33 }) {
            const auto x = renderExtended({ { 58, kd } }, 69, 1.0, 2.0, 1.5);
            const size_t want = (size_t)std::llround(extKeyOnDelayMs(kd) * rate / 1000);
            const size_t got = firstSound(x);
            expect(got >= want && got <= want + 1, "key-on delay " + std::to_string(kd) + ": first sound at " +
                                                       std::to_string(got) + ", expected " + std::to_string(want));
        }
        const auto sync = renderExtended({ { 58, 100 }, { 59, 1 }, { 96, 6 } }, 69, 1.0, 1.0, 0.8);
        expect(firstSound(sync) >= 24000 && firstSound(sync) <= 24001, "tempo-synced key-on delay 1/4 at 120: " +
                                                                           std::to_string(firstSound(sync)));
        const auto early = renderExtended({ { 58, 40 } }, 69, 1.0, 0.2, 2.0);
        expect(firstSound(early) == early.size(), "a note released within its key-on delay never sounds");
    }
    // -- pan: the far side's gain by position, the near side at full level; scaling by key;
    // alternate pan swaps sides on each note; unpanned sounds are the same in both channels
    {
        auto rms = [](const std::vector<float> &x) {
            double e = 0;
            for (size_t i = 4800; i < x.size(); i++)
                e += (double)x[i] * x[i];
            return std::sqrt(e / (double)(x.size() - 4800));
        };
        for (int ap : { -64, -20, 0, 32, 63 }) {
            std::vector<float> r;
            const auto l = renderExtended({ { 53, ap } }, 69, 1.0, 0.5, 0.5, &r);
            float gl, gr;
            extPanGains(ap, gl, gr);
            const bool ok = ap == 0 ? l == r
                                    : std::fabs(rms(l) * gr - rms(r) * gl) < 1e-4 * rms(ap < 0 ? l : r);
            expect(ok, "alternate pan " + std::to_string(ap) + ": left " + std::to_string(rms(l)) + ", right " +
                           std::to_string(rms(r)));
        }
        {
            std::vector<float> r;
            const auto l = renderExtended({ { 54, 63 } }, 84, 1.0, 0.5, 0.5, &r);
            float gl, gr;
            extPanGains(extScalingPanPosition(63, 84), gl, gr);
            expect(extScalingPanPosition(63, 84) == 40 && extScalingPanPosition(32, 62) == 0 &&
                       extScalingPanPosition(32, 48) == -12 && std::fabs(rms(l) / rms(r) - gl / gr) < 1e-3,
                   "scaling pan by key");
        }
        float gl, gr;
        extPanGains(32, gl, gr);
        expect(gr == 1.0f && std::fabs(20 * std::log10(gl) + 6.41) < 0.01, "pan law: -6.41 dB on the far side at 32");
        extPanGains(-64, gl, gr);
        expect(gl == 1.0f && gr == 0.0f, "pan law: hard left");
        // two notes of one sound in one host: the second goes to the other side
        Host h(entry);
        h.activate(48000, 512);
        EventList in;
        in.events.reserve(64);
        in.param(0, 5, 1);
        in.param(0, 53, -40);
        h.process(512, in);
        in.clear();
        double side[2][2] = {};
        for (int n = 0; n < 2; n++) {
            in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, 69, 1.0);
            for (int b = 0; b < 40; b++) {
                if (b == 30)
                    in.note(CLAP_EVENT_NOTE_OFF, 0, -1, 0, 69, 0);
                h.process(512, in);
                in.clear();
                if (b >= 10 && b < 30)
                    for (int i = 0; i < 512; i++) {
                        side[n][0] += (double)h.left[i] * h.left[i];
                        side[n][1] += (double)h.right[i] * h.right[i];
                    }
            }
        }
        expect(side[0][0] > 4 * side[0][1] && side[1][1] > 4 * side[1][0], "alternate pan: left, then right");
    }
    // -- the part LFO: rate, the waves' shapes and phase, the envelope's delay, the user wave's
    // steps and slopes, and through the plugin its operator frequency destination
    {
        ExtendedVoice v = oscairn_fm::extDefaults();
        v.v[ext::ProfileSel] = ext::Extended;
        v.v[ext::PartLfoDest1] = ext::PartLfoOpFrequency;
        v.v[ext::PartLfoDepth1] = 8;
        auto run = [&](std::vector<float> &out, size_t n) {
            ExtPartLfo l;
            l.init(48000);
            l.configure(v, 120);
            l.keydown(true);
            out.assign(n, 0);
            for (size_t i = 0; i < n; i += 64)
                l.render(out.data() + i, (int)std::min<size_t>(64, n - i));
        };
        std::vector<float> x;
        v.v[ext::PartLfoSpeed] = 48;
        run(x, 48000);
        const double period = 48000 / extPartLfoHz(48); // samples
        const double wait = 0.012 * 48000;              // the LFO starts 12 ms after the key-on
        auto at = [&](double cycles) { return x[(size_t)std::lround(wait + cycles * period)]; };
        expect(std::fabs(at(0.25) - 1) < 1e-3 && std::fabs(at(0.5)) < 1e-3 && std::fabs(at(0.75) + 1) < 1e-3 &&
                   std::fabs(at(1.25) - 1) < 1e-3,
               "part LFO triangle at its rate: " + std::to_string(at(0.25)) + " " + std::to_string(at(0.75)));
        const struct { int wave; double p; float want; } shapes[] = {
            { ExtPartLfo::TrianglePlus, 0.5, 1 }, { ExtPartLfo::TrianglePlus, 0.9, 0.2f }, { ExtPartLfo::SawUp, 0.6, 0.6f },
            { ExtPartLfo::SawDown, 0.6, -0.6f }, { ExtPartLfo::Square14, 0.2, 1 }, { ExtPartLfo::Square14, 0.3, -1 },
            { ExtPartLfo::Square34, 0.7, 1 }, { ExtPartLfo::Trapezoid, 0.1, 0.8f }, { ExtPartLfo::Trapezoid, 0.3, 1 } };
        for (const auto &sh : shapes) {
            v.v[ext::PartLfoWave] = sh.wave;
            run(x, 48000);
            expect(std::fabs(at(sh.p) - sh.want) < 2e-3, "part LFO wave " + std::to_string(sh.wave) + " at " +
                                                            std::to_string(sh.p) + ": " + std::to_string(at(sh.p)));
        }
        v.v[ext::PartLfoWave] = ExtPartLfo::Triangle;
        v.v[ext::PartLfoPhase] = 1; // 90 degrees: starts at the top
        run(x, 4800);
        expect(std::fabs(x[0] - 1) < 1e-3, "part LFO phase 90 degrees starts at the top");
        v.v[ext::PartLfoPhase] = 0;
        v.v[ext::PartLfoDelay] = 64;
        run(x, 48000);
        const size_t d = (size_t)std::lround(wait + extPartLfoDelayMs(64) * 48);
        expect(x[d - 2] == 0 && x[d + 480] != 0, "part LFO delay");
        v.v[ext::PartLfoDelay] = 0;
        // user wave, 4 steps: +63 (full), -64, +32, -32; slope up ramps towards a higher next step
        v.v[ext::PartLfoWave] = ExtPartLfo::User;
        v.v[ext::PartLfoUserCycle] = 2;
        const int steps[4] = { 63, -64, 32, -32 };
        for (int i = 0; i < 4; i++)
            v.v[ext::PartLfoStep1 + i] = (int16_t)steps[i];
        v.v[ext::PartLfoUserSlope] = 1;
        run(x, 48000);
        expect(std::fabs(at(0.1) - 1) < 1e-3 && std::fabs(at(0.375) - (-1 + 1.5 * 0.5)) < 2e-3 &&
                   std::fabs(at(0.6) - 0.5) < 1e-3,
               "part LFO user steps and slope up: " + std::to_string(at(0.1)) + " " + std::to_string(at(0.375)) + " " +
                   std::to_string(at(0.6)));
        // through the plugin: a square wave at depth 8 moves the operator by the table's cents
        const double cents = extPartLfoPitchCents(8);
        const auto y = renderExtended({ { 210, ext::PartLfoOpFrequency }, { 211, 8 }, { 201, ExtPartLfo::Square },
                                        { 202, 24 } }, 69, 1.0, 1.0, 1.0);
        const double up = 1200 * std::log2(zeroCrossingHz(y, 2400, 24000) / f69);
        expect(std::fabs(up - cents) < 1.0, "part LFO to operator frequency: " + std::to_string(up) + " cents, table " +
                                                std::to_string(cents));
        // operator level: a square wave at depth 32 lowers the level by the table in its bottom half
        {
            const auto z = renderExtended({ { 210, ext::PartLfoOpLevel }, { 211, 32 }, { 201, ExtPartLfo::Square },
                                            { 202, 24 } }, 69, 1.0, 2.5, 2.5);
            const double half = 0.5 / extPartLfoHz(24) * rate; // samples
            auto rmsDb = [&](double from, double to) {
                double e = 0;
                for (size_t i = (size_t)from; i < (size_t)to; i++)
                    e += (double)z[i] * z[i];
                return 10 * std::log10(e / (to - from));
            };
            const double drop = rmsDb(0.2 * half, 0.8 * half) - rmsDb(1.2 * half, 1.8 * half);
            expect(std::fabs(drop - extPartLfoLevelDb(32)) < 0.1, "part LFO to operator level: " + std::to_string(drop) +
                                                                   " dB, table " + std::to_string(extPartLfoLevelDb(32)));
        }
        // detune: a square wave at depth 32 moves the detune by 8 steps either way
        {
            const auto z = renderExtended({ { 210, ext::PartLfoOpDetune }, { 211, 32 }, { 201, ExtPartLfo::Square },
                                            { 202, 24 } }, 60, 1.0, 2.5, 2.5);
            const double half = 0.5 / extPartLfoHz(24) * rate;
            const double hz60 = noteHz(60);
            const double top = 1200 * std::log2(zeroCrossingHz(z, (size_t)(0.2 * half), (size_t)(0.9 * half)) / hz60);
            const double bottom = 1200 * std::log2(zeroCrossingHz(z, (size_t)(1.2 * half), (size_t)(1.9 * half)) / hz60);
            expect(std::fabs(top - extDetuneCents(8, 60)) < 0.3 && std::fabs(bottom - extDetuneCents(-8, 60)) < 0.3,
                   "part LFO to detune: " + std::to_string(top) + " / " + std::to_string(bottom) + " cents");
        }
        // feedback: a square wave at depth 127 from feedback 0 plays feedback 7 in its top half
        // (a strong second harmonic) and none in its bottom half
        {
            const auto z = renderExtended({ { 210, ext::PartLfoFeedback }, { 211, 127 }, { 201, ExtPartLfo::Square },
                                            { 202, 24 } }, 69, 1.0, 2.5, 2.5);
            const double half = 0.5 / extPartLfoHz(24) * rate;
            auto h2 = [&](double from, double to) { // second harmonic against the first, dB
                double re1 = 0, im1 = 0, re2 = 0, im2 = 0;
                for (size_t i = (size_t)from; i < (size_t)to; i++) {
                    const double ph = 2 * M_PI * f69 * i / rate;
                    re1 += z[i] * std::cos(ph); im1 += z[i] * std::sin(ph);
                    re2 += z[i] * std::cos(2 * ph); im2 += z[i] * std::sin(2 * ph);
                }
                return 10 * std::log10((re2 * re2 + im2 * im2) / (re1 * re1 + im1 * im1));
            };
            const double top = h2(0.2 * half, 0.9 * half), bottom = h2(1.2 * half, 1.9 * half);
            expect(top > -15 && bottom < -60, "part LFO to feedback: second harmonic " + std::to_string(top) + " / " +
                                                  std::to_string(bottom) + " dB");
        }
        // LFO 2 speed: depth 32 moves the 2nd LFO by 25 speed steps at the top
        {
            ExtendedVoice w = v;
            w.v[ext::PartLfoDest1] = ext::PartLfoLfo2Speed;
            w.v[ext::PartLfoDepth1] = 32;
            ExtPartLfo l;
            l.init(48000);
            l.configure(w, 120);
            expect(l.lfo2Steps(1.0f) == 25 && l.lfo2Steps(-1.0f) == -25 && l.lfo2Steps(0.5f) == 12,
                   "part LFO to LFO 2 speed: steps");
        }
        // operator wave: a square wave at depth 32 from all harmonics 1 plays odd harmonics 1 in
        // its top half (wave 3: no second harmonic) and a sine in its bottom half (an unwindowed
        // DFT over part-cycles, so "none" is below -40 dB here)
        {
            const auto z = renderExtended({ { 1060, 1 }, { 1061, 3 }, { 210, ext::PartLfoOpWave }, { 211, 32 },
                                            { 201, ExtPartLfo::Square }, { 202, 24 } }, 57, 1.0, 2.5, 2.5);
            const double half = 0.5 / extPartLfoHz(24) * rate, f57 = f69 / 2;
            auto h = [&](int k, double from, double to) {
                double re = 0, im = 0, re1 = 0, im1 = 0;
                for (size_t i = (size_t)from; i < (size_t)to; i++) {
                    const double ph = 2 * M_PI * f57 * i / rate;
                    re += z[i] * std::cos(k * ph); im += z[i] * std::sin(k * ph);
                    re1 += z[i] * std::cos(ph); im1 += z[i] * std::sin(ph);
                }
                return 10 * std::log10((re * re + im * im) / (re1 * re1 + im1 * im1));
            };
            expect(h(2, 0.2 * half, 0.9 * half) < -40 && h(3, 0.2 * half, 0.9 * half) > -20 && h(3, 1.2 * half, 1.9 * half) < -40,
                   "part LFO to operator wave: h2 " + std::to_string(h(2, 0.2 * half, 0.9 * half)) + " h3 " +
                       std::to_string(h(3, 0.2 * half, 0.9 * half)) + " / " + std::to_string(h(3, 1.2 * half, 1.9 * half)));
        }
        // pan: a square wave at depth 32 puts the note right in its top half, left in its bottom
        {
            std::vector<float> r;
            const auto l = renderExtended({ { 210, ext::PartLfoPan }, { 211, 32 }, { 201, ExtPartLfo::Square }, { 202, 24 } },
                                          69, 1.0, 2.5, 2.5, &r);
            const double half = 0.5 / extPartLfoHz(24) * rate;
            auto db = [](const std::vector<float> &x, double from, double to) {
                double e = 0;
                for (size_t i = (size_t)from; i < (size_t)to; i++)
                    e += (double)x[i] * x[i];
                return 10 * std::log10(e / (to - from));
            };
            float gl, gr;
            extPanGains((int)std::lround(extPartLfoPanPositions(32)), gl, gr);
            const double top = db(l, 0.2 * half, 0.8 * half) - db(r, 0.2 * half, 0.8 * half);
            const double bottom = db(r, 1.2 * half, 1.8 * half) - db(l, 1.2 * half, 1.8 * half);
            const double want = 20 * std::log10(gl);
            expect(std::fabs(top - want) < 0.05 && std::fabs(bottom - want) < 0.05,
                   "part LFO to pan: " + std::to_string(top) + " / " + std::to_string(bottom) + " dB, law " + std::to_string(want));
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
        // formant 2 at spread 5, formant 0: harmonic 15 is where the window's components below
        // harmonic 1 fold back onto those above it; past the main lobe's null they are negative
        // (the reference's `spec` render: -75.75 dB against a sine; -82.9 with them positive)
        const auto f = renderExtended({ { 1060, 6 }, { 1061, 5 } }, 48, 1.0, 1.1, 1.1);
        const double h15 = 20 * std::log10(harmonic(f, f0 * 15) / sine);
        expect(std::fabs(h15 + 75.75) < 1.5, "formant 2 spread 5 harmonic 15: " + std::to_string(h15) + " dB, the reference's -75.75");
    }
    // -- where the waves sit in their cycle (it shows when they modulate): waves 1 and 2 run from 0
    // and peak half a cycle in, waves 3 and 4 peak a quarter cycle in, formant 2's window peaks
    // half a cycle in
    {
        extwave::init();
        auto peakAt = [](const int32_t *t) { return (int)(std::max_element(t, t + extwave::kSize) - t); };
        const int slack = extwave::kSize / 256;
        for (int w = 1; w <= 4; w++)
            for (int s = 0; s < 8; s++) {
                const int32_t *t = extwave::table(w, s, 512);
                const int at = peakAt(t), want = w <= 2 ? extwave::kSize / 2 : extwave::kSize / 4;
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
    // -- a modulated non-sine carrier (`m9a`, note 48, a sine at ratio 1 and level 65 modulating
    // operator 2): all harmonics 2 at spread 4 plays its wave half a cycle on, odd harmonics 2 at
    // spread 2 E(2 t + m) sin(t + m), formant 1 at formant 5 window(t + m) sin(6 t + m); harmonics
    // 1-8 relative to the strongest against the reference's, within 1 dB where it is within 40 dB
    {
        using P = std::vector<std::pair<uint32_t, double>>;
        auto harmonics = [&](const std::vector<float> &x) {
            std::vector<double> db;
            const size_t a = 9600, n = 19200;
            for (int k = 1; k <= 8; k++) {
                double re = 0, im = 0;
                for (size_t i = 0; i < n; i++) {
                    const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n), ph = 2 * M_PI * noteHz(48) * k * i / rate;
                    re += w * x[a + i] * std::cos(ph);
                    im += w * x[a + i] * std::sin(ph);
                }
                db.push_back(20 * std::log10(std::hypot(re, im) + 1e-30));
            }
            const double top = *std::max_element(db.begin(), db.end());
            for (auto &d : db)
                d -= top;
            return db;
        };
        const struct { int wave, spread, formant; double ref[8]; } cases[] = {
            { 2, 4, 0, { 0.0, -5.2, -16.5, -30.9, -28.1, -42.3, -47.0, -55.2 } },
            { 4, 2, 0, { 0.0, -6.4, -6.2, -6.1, -31.0, -13.5, -21.7, -32.7 } },
            { 5, 0, 5, { -89.3, -67.1, -45.6, -26.4, -2.7, 0.0, -24.6, -15.2 } },
        };
        for (const auto &c : cases) {
            const auto db = harmonics(renderExtended(P{ { 40, 8 }, { 1051, 65 }, { 2051, 99 }, { 1032, 1 }, { 2032, 1 },
                                                        { 2060, c.wave }, { 2061, c.spread }, { 2062, c.formant } },
                                                     48, 1.0, 0.7, 0.7));
            double worst = 0;
            for (int k = 0; k < 8; k++)
                if (c.ref[k] > -40)
                    worst = std::max(worst, std::fabs(db[k] - c.ref[k]));
            std::printf("modulated wave %d spread %d formant %d: harmonics within %.2f dB of the reference's\n", c.wave,
                        c.spread, c.formant, worst);
            expect(worst < 1.0, "modulated wave " + std::to_string(c.wave) + " spread " + std::to_string(c.spread) +
                                    ": " + std::to_string(worst) + " dB from the reference");
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
    // -- the 2nd LFO's delay: its fade in takes as long as its hold (at 99 several seconds)
    for (int delay : { 40, 70, 99 }) {
        ExtendedVoice v = oscairn_fm::extDefaults();
        v.v[ext::LfoDelay] = (int16_t)delay;
        Lfo lfo;
        lfo.init(rate);
        lfo.reset(v);
        lfo.keydown();
        long hold = -1, full = -1;
        for (long b = 0; b < 100000 && full < 0; b++) {
            const int32_t d = lfo.getdelay();
            if (d > 0 && hold < 0)
                hold = b;
            if (d >= (1 << 24))
                full = b;
        }
        expect(hold > 0 && std::labs(full - 2 * hold) <= 2, "2nd LFO delay " + std::to_string(delay) + ": hold " +
                                                                 std::to_string(hold) + " blocks, full at " + std::to_string(full));
    }
    // -- the 2nd LFO runs per voice: with key-on reset a note started while another is held
    // plays as it would alone and leaves the held note's vibrato unchanged (with a delay too),
    // so the two notes' output is the sum of each alone
    for (int delay : { 0, 50 }) {
        const std::vector<std::pair<uint32_t, double>> p = { { 49, 0 }, { 43, 20 }, { 47, 30 }, { 46, 1 },
                                                             { 45, (double)delay }, { 1058, 7 } };
        const auto a = renderMidiNotes(p, { { 0.0, 36, 100 } }, 1.5);
        const auto c = renderMidiNotes(p, { { 0.53, 84, 100 } }, 1.5);
        const auto b = renderMidiNotes(p, { { 0.0, 36, 100 }, { 0.53, 84, 100 } }, 1.5);
        double worst = 0, peak = 0;
        for (size_t i = 0; i < b.size(); i++) {
            worst = std::max(worst, (double)std::fabs(b[i] - a[i] - c[i]));
            peak = std::max(peak, (double)std::fabs(c[i]));
        }
        expect(peak > 0.01 && worst < 1e-4, "2nd LFO per voice (delay " + std::to_string(delay) +
                                                "): two notes differ from each alone by " + std::to_string(worst));
    }
    // its pitch depth (twice the DX7's; depth 99 and offset 7 play as 256) and its start (phase 0
    // half a cycle into Lfo's waves: a square starts low), through the plugin
    for (const auto &d : { std::array<int, 3> { 30, 7, 77 * 256 }, std::array<int, 3> { 99, 7, 256 * 256 },
                           std::array<int, 3> { 99, 4, 256 * 56 } }) {
        const auto x = renderMidiNotes({ { 49, 0 }, { 43, 10 }, { 42, 3 }, { 46, 1 }, { 47, (double)d[0] },
                                         { 1058, (double)d[1] } },
                                       { { 0.0, 60, 100 } }, 0.7);
        const double half = 0.5 / extLfoHz(10), want = d[2] * 1200.0 / 32768;
        auto cents = [&](double a, double b) {
            return 1200 * std::log2(zeroCrossingHz(x, (size_t)(a * half * rate), (size_t)(b * half * rate)) / noteHz(60));
        };
        const double lo = cents(0.1, 0.9), hi = cents(1.1, 1.9);
        expect(std::fabs(lo + want) < 1 && std::fabs(hi - want) < 1,
               "2nd LFO pitch depth " + std::to_string(d[0]) + " offset " + std::to_string(d[1]) + ": " +
                   std::to_string(lo) + " then " + std::to_string(hi) + " cents, want -+" + std::to_string(want));
    }
    // its amplitude depth: straight in dB from the LFO's top, 0.375 dB steps by depth (50: 32 steps
    // at offset 7) and eighths by offset (3: half), through the plugin with a square wave (low first);
    // below about -45 dB bent as a carrier's EG levels are (depth 99, offsets 3 and 4: 48 and 60 dB
    // straight play as 48.5 and 62.64)
    for (const auto &d : { std::array<double, 3> { 50, 7, 12.0 }, std::array<double, 3> { 50, 3, 6.0 },
                           std::array<double, 3> { 85, 7, 33.375 }, std::array<double, 3> { 99, 3, 48.50 },
                           std::array<double, 3> { 99, 4, 62.64 } }) {
        const auto x = renderMidiNotes({ { 49, 0 }, { 43, 10 }, { 42, 3 }, { 46, 1 }, { 48, d[0] }, { 1059, d[1] } },
                                       { { 0.0, 60, 100 } }, 0.7);
        const double half = 0.5 / extLfoHz(10);
        const auto db = peakDb(x, 960);
        auto at = [&](double t) { return db[(size_t)(t * half * rate / 960)]; };
        const double swing = at(1.5) - at(0.5);
        expect(std::fabs(swing - d[2]) < 0.1, "2nd LFO amplitude depth " + std::to_string((int)d[0]) + " offset " +
                                                  std::to_string((int)d[1]) + ": " + std::to_string(swing) + " dB, want " +
                                                  std::to_string(d[2]));
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
                const int levels[5] = { 0, lv, 0, 0, 0 };
                const double times[5] = { 0, 0, (double)t, 0, 0 };
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
        // time velocity sensitivity moves the time value by sensitivity x (velocity - 1) / 127 on the
        // segments its segment value names; any negative sensitivity makes them instant above
        // velocity 1. Through the plugin: HPF12 at cutoff 512 on a sine at note 36, the FEG's decay 1
        // from +1200 cents (time 64), timed to where the level settles within 0.5 dB
        {
            auto settle = [&](const std::vector<std::pair<uint32_t, double>> &extra, double velocity) {
                std::vector<std::pair<uint32_t, double>> p = { { 1048, 0 }, { 60, Hpf12 }, { 61, 512 }, { 63, 0 }, { 85, 0 },
                                                              { 78, 63 }, { 74, 16 }, { 70, 64 } };
                p.insert(p.end(), extra.begin(), extra.end());
                const auto x = renderExtended(p, 36, velocity, 1.5, 1.5);
                std::vector<double> db;
                for (size_t i = 0; i + 960 <= x.size(); i += 240) {
                    double e = 0;
                    for (size_t j = i; j < i + 960; j++)
                        e += (double)x[j] * x[j];
                    db.push_back(10 * std::log10(e / 960 + 1e-30));
                }
                size_t k = 0;
                while (k < db.size() && std::fabs(db[k] - db.back()) > 0.5)
                    k++;
                return std::make_pair(x, k * 240 / 48.0); // (ms)
            };
            const auto plain = settle({}, 96 / 127.0);
            const auto v96 = settle({ { 80, 32 } }, 96 / 127.0), t40 = settle({ { 70, 40 } }, 96 / 127.0);
            expect(plain.second > 600 && std::fabs(v96.second - t40.second) <= 5,
                   "FEG time velocity +32 at velocity 96 plays decay 1 as time 40 (" + std::to_string(v96.second) +
                       " ms, time 40 " + std::to_string(t40.second) + " ms, time 64 " + std::to_string(plain.second) + " ms)");
            const auto neg = settle({ { 80, -8 } }, 64 / 127.0), t0 = settle({ { 70, 0 } }, 64 / 127.0);
            expect(std::fabs(neg.second - t0.second) <= 5, "FEG time velocity -8 at velocity 64 makes decay 1 instant (" +
                                                                std::to_string(neg.second) + " ms)");
            expect(settle({ { 80, 63 } }, 1 / 127.0).first == settle({}, 1 / 127.0).first,
                   "FEG time velocity leaves velocity 1 alone");
            expect(settle({ { 80, 32 }, { 79, 0 } }, 96 / 127.0).first == plain.first,
                   "FEG time velocity on the attack only leaves decay 1 alone");
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
        // the part LFO's cutoff destination moves the cutoff value by 8 per depth step: a square
        // wave at depth 8 plays HPF12 at cutoff 512 + 64 in its top half and 512 - 64 in its bottom
        {
            const auto x = renderExtended({ { 1048, 0 }, { 60, Hpf12 }, { 61, 512 }, { 63, 0 }, { 85, 0 },
                                            { 210, ext::PartLfoCutoff }, { 211, 8 }, { 201, 6 }, { 202, 24 } },
                                          36, 1.0, 2.5, 2.5);
            const auto t = renderExtended({ { 1048, 0 } }, 36, 1.0, 1.0, 1.0);
            const double half = 0.5 / extPartLfoHz(24) * 48000;
            auto db = [](const std::vector<float> &y, double from, double to) {
                double e = 0;
                for (size_t i = (size_t)from; i < (size_t)to; i++)
                    e += (double)y[i] * y[i];
                return 10 * std::log10(e / (to - from));
            };
            auto model = [&](int value) {
                float F, d;
                extFilterCoefficients(extFilterCutoffHz(value), 0.5, 48000, F, d);
                const double f = 440 * std::exp2((36 - 69) / 12.0);
                const C zi = std::polar(1.0, -2 * M_PI * f / 48000), a = 1.0 - zi;
                return 20 * std::log10(std::abs(1.0 / (1.0 + (double)F * (double)F * zi / (a * a) + (double)d * zi * (double)F / a)));
            };
            const double thru = db(t, 12000, 36000);
            const double top = db(x, 0.3 * half, 0.9 * half) - thru, bottom = db(x, 1.3 * half, 1.9 * half) - thru;
            expect(std::fabs(top - model(576)) < 0.2 && std::fabs(bottom - model(448)) < 0.2,
                   "part LFO to cutoff: " + std::to_string(top) + " / " + std::to_string(bottom) + " dB, model " +
                       std::to_string(model(576)) + " / " + std::to_string(model(448)));
        }
        // the resonance destination: one resonance step per depth step (resonance 40 +- 8)
        {
            const auto x = renderExtended({ { 1048, 0 }, { 60, Hpf12 }, { 61, 512 }, { 63, 40 }, { 85, 0 },
                                            { 210, ext::PartLfoResonance }, { 211, 8 }, { 201, 6 }, { 202, 24 } },
                                          36, 1.0, 2.5, 2.5);
            const auto t = renderExtended({ { 1048, 0 } }, 36, 1.0, 1.0, 1.0);
            const double half = 0.5 / extPartLfoHz(24) * 48000;
            auto db = [](const std::vector<float> &y, double from, double to) {
                double e = 0;
                for (size_t i = (size_t)from; i < (size_t)to; i++)
                    e += (double)y[i] * y[i];
                return 10 * std::log10(e / (to - from));
            };
            auto model = [&](int res) {
                float F, d;
                extFilterCoefficients(extFilterCutoffHz(512), extFilterQ(res), 48000, F, d);
                const double f = 440 * std::exp2((36 - 69) / 12.0);
                const C zi = std::polar(1.0, -2 * M_PI * f / 48000), a = 1.0 - zi;
                return 20 * std::log10(std::abs(1.0 / (1.0 + (double)F * (double)F * zi / (a * a) + (double)d * zi * (double)F / a)));
            };
            const double thru = db(t, 12000, 36000);
            const double top = db(x, 0.3 * half, 0.9 * half) - thru, bottom = db(x, 1.3 * half, 1.9 * half) - thru;
            expect(std::fabs(top - model(48)) < 0.2 && std::fabs(bottom - model(32)) < 0.2,
                   "part LFO to resonance: " + std::to_string(top) + " / " + std::to_string(bottom) + " dB, model " +
                       std::to_string(model(48)) + " / " + std::to_string(model(32)));
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
    // -- overall level: a full-level Extended carrier in algorithm 1 plays 0.42 dB above the
    // Classic init voice's (the reference at part volume 127 without its centre pan dip)
    {
        auto rmsDb = [](const std::vector<float> &x) {
            double s = 0;
            for (size_t i = x.size() / 4; i < x.size() / 2; i++)
                s += (double)x[i] * x[i];
            return 10 * std::log10(s / (double)(x.size() / 2 - x.size() / 4));
        };
        const double ext = rmsDb(renderExtended({}, 69, 1.0, 1.0, 1.0));
        const double classic = rmsDb(renderExtended({ { 5, 0 } }, 69, 1.0, 1.0, 1.0));
        std::printf("Extended carrier %.3f dB re Classic\n", ext - classic);
        expect(std::fabs(ext - classic - 0.42) < 0.05,
               "Extended carrier level " + std::to_string(ext - classic) + " dB re Classic, want +0.42");
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
        // the 2nd LFO's pitch depth converts by its swing (the Extended depth is twice as deep)
        at(12) = 50;
        at(16) = 6;
        oscairn_fm::convertToExtended(v);
        const double classicCents = 128 * 153 * 1200.0 / 65536, extCents = extLfoPitchCents((int)at(47), (int)at(8058));
        expect(std::fabs(extCents / classicCents - 1) < 0.01, "LFO pitch depth 50 converts to " + std::to_string(at(47)) +
                                                                  ": " + std::to_string(extCents) + " cents, Classic " +
                                                                  std::to_string(classicCents));
        expect(oscairn_fm::convertToClassic(v) && at(12) == 50 && at(16) == 6, "LFO pitch depth survives the round trip");
        oscairn_fm::convertToExtended(v);
        at(8049) = 10; // a hold segment: not in Classic
        std::vector<double> before = v;
        expect(!oscairn_fm::convertToClassic(v) && v == before, "a sound outside the Classic subset stays Extended");
    }
    // -- no 16-bit rounding of Extended voices: a quiet sine's harmonics stay far below the
    // rounding error's level
    {
        const auto x = renderExtended({ { 1051, 50 } }, 69, 1.0, 1.2, 1.2);
        auto level = [&](double hz) {
            const size_t from = 24000, n = 24000;
            double re = 0, im = 0;
            for (size_t i = 0; i < n; i++) {
                const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n), a = 2 * M_PI * hz * i / rate;
                re += x[from + i] * w * std::cos(a);
                im += x[from + i] * w * std::sin(a);
            }
            return 20 * std::log10(std::hypot(re, im) + 1e-30);
        };
        const double fundamental = level(f69);
        double worst = -300;
        for (int k = 2; k <= 8; k++)
            worst = std::max(worst, level(f69 * k) - fundamental);
        std::printf("quiet sine: harmonics 2-8 at most %.1f dB\n", worst);
        expect(worst < -90, "quiet Extended sine: harmonics 2-8 at " + std::to_string(worst) + " dB");
    }
    // -- a rising decay segment approaches a point extAttackOverDb above full level exponentially
    // in dB at its rate in doublings per second, as the attack does (decay 1 to EG 70 at once, then
    // decay 2 to 99 at time 55): its level 0.1 and 0.2 s in, against the steady level, within 0.5 dB
    {
        using P = std::vector<std::pair<uint32_t, double>>;
        const auto x = renderExtended(P{ { 1041, 99 }, { 1042, 70 }, { 1043, 99 }, { 1045, 0 }, { 1046, 0 }, { 1047, 55 } },
                                      60, 1.0, 1.6, 1.6);
        size_t on = 0;
        while (on < x.size() && std::fabs(x[on]) < 1e-4f)
            on++;
        auto db = [&](double t) {
            const size_t a = on + (size_t)(t * rate), n = (size_t)(0.01 * rate);
            double e = 0;
            for (size_t i = 0; i < n; i++)
                e += (double)x[a + i] * x[a + i];
            return 10 * std::log10(e / n);
        };
        const double steady = db(1.4), r = 1000.0 / extEgMsPerDb(55) / 6.0206;
        for (double t : { 0.1, 0.2 }) {
            const double want = extAttackOverDb() - (extAttackOverDb() - extEgLevelDb(70)) * std::exp(-r * (t + 0.005));
            const double got = db(t) - steady;
            std::printf("rising decay 2 at %.1f s: %.2f dB (curve %.2f)\n", t, got, want);
            expect(std::fabs(got - want) < 0.5, "rising decay 2 at " + std::to_string(t) + " s: " + std::to_string(got) +
                                                    " dB, curve " + std::to_string(want));
        }
    }
    // -- a modulator's EG level: in steps of two, adding to its operator level as values (EG 85 at
    // level 70 plays as EG 99 at level 56); a carrier's EG keeps its own curve, also in steps of two
    {
        auto harmonics = [&](const std::vector<float> &x) {
            std::vector<double> db;
            const size_t a = 9600, n = 9600;
            for (int k = 1; k <= 6; k++) {
                double re = 0, im = 0;
                for (size_t i = 0; i < n; i++) {
                    const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n), ph = 2 * M_PI * noteHz(60) * k * i / rate;
                    re += w * x[a + i] * std::cos(ph);
                    im += w * x[a + i] * std::sin(ph);
                }
                db.push_back(20 * std::log10(std::hypot(re, im) + 1e-30));
            }
            return db;
        };
        using P = std::vector<std::pair<uint32_t, double>>;
        // (the carrier at level 90 keeps the output about where this check was set up, 7 dB under
        // a level 99 carrier since Extended voices play 6.99 dB louder: harmonic 6 lies about
        // 129 dB under the fundamental, where the comparison moves by a tenth of a dB with rounding)
        auto pair = [&](int level, int eg) {
            return renderExtended(P{ { 40, 8 }, { 1051, level }, { 2051, 90 }, { 1041, eg }, { 1042, eg }, { 1043, eg } }, 60,
                                  1.0, 0.4, 0.4);
        };
        expect(pair(70, 84) == pair(70, 85), "modulator EG level 84 plays as 85");
        const auto a = harmonics(pair(70, 85)), b = harmonics(pair(56, 99));
        double worst = 0;
        for (size_t k = 0; k < a.size(); k++)
            worst = std::max(worst, std::fabs(a[k] - b[k]));
        std::printf("modulator level 70 at EG 85 against level 56: harmonics 1-6 within %.3f dB\n", worst);
        expect(worst < 0.1, "modulator level 70 at EG 85 against level 56 at EG 99: " + std::to_string(worst) + " dB");
        // feedback 7 on the level 70 modulator: harmonics 2-6 fall a straight 6.8 dB each, as the
        // reference's do (`m7c` df7, the DX7's feedback amount on a modulator)
        {
            const auto f = harmonics(renderExtended(P{ { 40, 8 }, { 41, 7 }, { 1051, 70 }, { 2051, 90 } }, 60, 1.0, 0.4, 0.4));
            const double ref[5] = { -7.1, -14.5, -21.6, -28.4, -35.1 };
            double fw = 0;
            for (int k = 0; k < 5; k++)
                fw = std::max(fw, std::fabs(f[k + 1] - f[0] - ref[k]));
            std::printf("feedback 7 on a modulator: harmonics 2-6 within %.2f dB of the reference's\n", fw);
            expect(fw < 0.5, "feedback 7 on a modulator: " + std::to_string(fw) + " dB from the reference's harmonics");
        }
        auto carrier = [&](int eg) { return renderExtended(P{ { 1041, eg }, { 1042, eg }, { 1043, eg } }, 60, 1.0, 0.4, 0.4); };
        expect(carrier(84) == carrier(85) && carrier(85) != carrier(86),
               "carrier EG level 84 plays as 85, 86 differs");
        expect(extEgLevelDb(98) == 0.0 && std::fabs(extEgLevelDb(90) + 6.02) < 1e-9 && extEgLevelDb(19) < extEgLevelDb(20),
               "carrier EG level table");
        // the ratio macro: a modulator's ratio by its table (ratio 1 at +44 plays ratio 3), not a
        // carrier's
        auto near = [](double a, double b) { return std::fabs(a / b - 1) < 1e-3; };
        expect(near(extShiftedRatio(1, 44), 3.0) && near(extShiftedRatio(4, 99), 21.332) &&
                   near(extShiftedRatio(1, -99), 0.5) && near(extShiftedRatio(16, -99), 13.861) &&
                   near(extShiftedRatio(20, 99), 20 + 33.347 - 16) && extShiftedRatio(2.5, 0) == 2.5,
               "ratio macro table");
        const P tone = { { 40, 8 }, { 1051, 60 }, { 2051, 99 }, { 1032, 1 }, { 2032, 1 } };
        auto withRatio = [&](P p, const P &more) {
            p.insert(p.end(), more.begin(), more.end());
            return renderExtended(p, 60, 1.0, 0.4, 0.4);
        };
        const auto shifted = harmonics(withRatio(tone, { { 247, 44 } })), three = harmonics(withRatio(tone, { { 1032, 3 } }));
        double off = 0;
        for (size_t k = 0; k < shifted.size(); k++)
            if (three[k] > three[0] - 60) // (the others are numerical noise)
                off = std::max(off, std::fabs(shifted[k] - three[k]));
        expect(off < 0.05, "ratio macro +44 on a modulator at ratio 1 plays ratio 3: " + std::to_string(off) + " dB");
        expect(withRatio({}, { { 247, 99 } }) == withRatio({}, {}), "the ratio macro leaves a carrier alone");
        // the texture macro: negative values give carriers a wave, positive ones modulators
        expect(withRatio({}, { { 252, -40 } }) == withRatio({}, { { 1060, 6 }, { 1062, 3 } }),
               "texture -40 on a carrier: the formant wave at formant 3");
        expect(withRatio({}, { { 252, -20 } }) == withRatio({}, { { 1060, 2 }, { 1061, 6 } }),
               "texture -20 on a carrier: all harmonics 2 at spread 6");
        expect(withRatio({}, { { 252, 40 } }) == withRatio({}, {}), "positive texture leaves a carrier alone");
        expect(withRatio(tone, { { 252, 6 } }) == withRatio(tone, { { 1060, 3 }, { 1061, 2 } }),
               "texture +6 on a modulator: odd harmonics 1 at spread 2");
        expect(withRatio(tone, { { 252, -6 } }) == withRatio(tone, { { 2060, 4 }, { 2061, 2 } }),
               "texture -6 on the pair reaches the carrier only: odd harmonics 2 at spread 2");
        int tw = 0, ts = 0, tf = 0;
        double tdb = 0;
        const bool high = extTextureWave(-99, false, tw, ts, tf, tdb) && tw == 6 && ts == 7 && tf == 99;
        tw = ts = tf = 0;
        const bool dip = extTextureWave(-32, false, tw, ts, tf, tdb) && tw == 0 && tdb < -6;
        expect(high && dip && !extTextureWave(1, false, tw, ts, tf, tdb) && extTextureWave(1, true, tw, ts, tf, tdb) && tw == 0,
               "texture table");
        // an operator's own wave is its starting place: all harmonics 1 at spread 3 sits at +26
        int pw = 1, ps = 3, pf = 0;
        const bool toSine = extTextureWave(-25, false, pw, ps, pf, tdb) && pw == 0;
        pw = 1, ps = 3, pf = 0;
        const bool toAll2 = extTextureWave(-50, false, pw, ps, pf, tdb) && pw == 2 && ps == 4;
        expect(toSine && toAll2, "texture -25 and -50 on a carrier with all harmonics 1 at spread 3: a sine, all harmonics 2 at spread 4");
        // the plus side's formant wave is formant 1; a formant 2 operator sits on the minus side
        // whichever it is, and a place past an end comes round from the other
        int qw = 0, qs = 0, qf = 0;
        const bool plusFormant = extTextureWave(60, true, qw, qs, qf, tdb) && qw == 5 && qs == 6 && qf == 7;
        qw = 6, qs = 0, qf = 3; // place -40
        const bool fromMinus = extTextureWave(50, true, qw, qs, qf, tdb) && qw == 3 && qs == 4;
        qw = 6, qs = 7, qf = 50; // place -90
        const bool round = extTextureWave(-20, false, qw, qs, qf, tdb) && qw == 5 && qs == 7 && qf == 46;
        qw = 6, qs = 0, qf = 0;
        const bool down = extTextureWave(-40, false, qw, qs, qf, tdb) && qw == 6 && qs == 7 && qf == 18;
        expect(plusFormant && fromMinus && round && down,
               "texture places: +60 formant 1, formant 2 from -40 by +50, -90 by -20 round to +89, -34 by -40");
    }
    // -- part values and macros: each plays as the operator or filter values it stands for
    {
        using P = std::vector<std::pair<uint32_t, double>>;
        auto same = [&](const P &a, const P &b, const std::string &what, int key = 60, double vel = 1.0) {
            expect(renderExtended(a, key, vel, 0.6, 0.8) == renderExtended(b, key, vel, 0.6, 0.8), what);
        };
        const P stack = { { 40, 8 }, { 1051, 70 }, { 2051, 99 }, { 1045, 40 }, { 1046, 50 }, { 1047, 55 }, { 1043, 75 },
                          { 1048, 50 }, { 2045, 50 }, { 2046, 55 }, { 2047, 60 }, { 2043, 80 }, { 2048, 55 } };
        auto with = [&](P p, const P &more) {
            for (const auto &m : more) {
                bool found = false;
                for (auto &q : p)
                    if (q.first == m.first) {
                        q.second = m.second;
                        found = true;
                    }
                if (!found)
                    p.push_back(m);
            }
            return p;
        };
        same(with(stack, { { 246, -25 } }), with(stack, { { 1051, 45 } }), "mod depth -25: modulator level 70 -> 45");
        same(with(stack, { { 41, 4 }, { 246, -99 } }), with(stack, { { 41, 4 }, { 1051, 51 } }),
             "mod depth -99 on the feedback operator at feedback 4: level 70 -> 51");
        same(with(stack, { { 248, 20 }, { 249, -10 }, { 250, 10 }, { 251, 30 } }),
             with(stack, { { 1045, 60 }, { 1046, 40 }, { 1047, 45 }, { 1043, 85 }, { 1048, 80 } }),
             "envelope macros move the modulator's attack, decays, sustain level and release only");
        same(with(stack, { { 238, -8 }, { 239, 6 }, { 240, -2 } }),
             with(stack, { { 1046, 38 }, { 1047, 43 }, { 2046, 43 }, { 2047, 48 }, { 1043, 83 }, { 2043, 88 }, { 1048, 47 },
                           { 2048, 52 } }),
             "amp EG offsets: decays and release 1.5 values per step, sustain by its table (+6: 8), on every operator");
        expect(extAegSustainSteps(-16) == -26 && extAegSustainSteps(16) == 24 && extAegSustainSteps(3) == 4 &&
                   extAegSustainSteps(-30) == -45,
               "sustain offset table");
        // the attack offset multiplies the attack time by 2^(offset / 4): +8 takes four times as long
        // to come within 3 dB of the top, on a carrier
        {
            auto reach = [&](const std::vector<float> &x) {
                const auto db = peakDb(x, 48);
                const double top = *std::max_element(db.begin(), db.end());
                for (size_t i = 0; i < db.size(); i++)
                    if (db[i] > top - 3)
                        return (double)i;
                return -1.0;
            };
            const P slow = { { 1045, 40 }, { 1042, 99 }, { 1043, 99 } };
            const double a0 = reach(renderExtended(slow, 60, 1.0, 1.5, 1.5)),
                         a8 = reach(renderExtended(with(slow, { { 237, 8 } }), 60, 1.0, 1.5, 1.5));
            expect(a0 > 3 && std::fabs(a8 / a0 - 4) < 0.25, "attack offset +8: " + std::to_string(a8 / a0) + " times as long");
        }
        same({ { 1071, 1 }, { 242, -8 }, { 60, 7 }, { 61, 628 } }, { { 1071, 1 }, { 60, 7 }, { 61, 564 } },
             "cutoff offset: 8 values per step");
        same({ { 243, 8 }, { 60, 7 }, { 61, 628 }, { 63, 64 } }, { { 60, 7 }, { 61, 628 }, { 63, 80 } },
             "resonance offset: 2 values per step");
        same({ { 241, -8 }, { 60, 7 }, { 61, 628 }, { 78, 32 }, { 74, 16 } }, { { 60, 7 }, { 61, 628 }, { 78, 16 }, { 74, 16 } },
             "filter EG depth offset: 2 values per step");
        expect(renderExtended({ { 244, 12 } }, 48, 1.0, 0.6, 0.8) == renderExtended({}, 60, 1.0, 0.6, 0.8),
               "note shift 12 at key 48 plays key 60");
        expect(renderExtended({ { 1057, 7 }, { 236, 32 } }, 60, 1.0, 0.6, 0.8) ==
                   renderExtended({ { 1057, 7 } }, 60, 63 / 127.0, 0.6, 0.8) &&
               renderExtended({ { 1057, 7 } }, 60, 1.0, 0.6, 0.8) != renderExtended({ { 1057, 7 } }, 60, 63 / 127.0, 0.6, 0.8),
               "velocity offset 32: velocity 127 plays as 63");
        {
            // fractional part velocities play between their neighbours' levels (64.5 here), and
            // below 1 by +7's measured curve (0.5: 12.25 dB below velocity 1)
            auto level = [&](const std::vector<std::pair<uint32_t, double>> &p, int vel) {
                const auto x = renderExtended(p, 60, vel / 127.0, 0.6, 0.6);
                double e = 0;
                for (size_t i = 4800; i < 24000; i++)
                    e += (double)x[i] * x[i];
                return 10 * std::log10(e / 19200 + 1e-30);
            };
            const double l64 = level({ { 1057, 7 } }, 64), l65 = level({ { 1057, 7 } }, 65),
                         lh = level({ { 1057, 7 }, { 235, 96 } }, 43);
            expect(lh > std::min(l64, l65) + 0.01 && lh < std::max(l64, l65) - 0.01,
                   "velocity depth 96: velocity 43 plays between 64 and 65");
            const double one = level({ { 1057, 7 } }, 1), half = level({ { 1057, 7 }, { 235, 1 } }, 32);
            expect(std::fabs(half - one + 12.25) < 0.1, "part velocity 0.5 at +7: " + std::to_string(half - one) + " dB");
        }
        expect(!audible(renderExtended({ { 235, 0 } }, 60, 1.0, 0.6, 0.8)), "velocity depth 0: no note");
        // operator level velocity sensitivity: the measured table, against sensitivity 0
        {
            auto rms = [&](int sens, int vel) {
                const auto x = renderExtended({ { 1057, sens } }, 60, vel / 127.0, 0.6, 0.6);
                double e = 0;
                for (size_t i = 4800; i < 24000; i++)
                    e += (double)x[i] * x[i];
                return 10 * std::log10(e / 19200 + 1e-30);
            };
            const double ref = rms(0, 100);
            for (const auto &c : std::vector<std::array<double, 3>>{ { 7, 1, -59.11 }, { 7, 127, 5.27 }, { 3, 64, -4.52 }, { -4, 104, -16.57 } })
                expect(std::fabs(rms((int)c[0], (int)c[1]) - ref - c[2]) < 0.2,
                       "level velocity " + std::to_string((int)c[0]) + " at " + std::to_string((int)c[1]) + ": " +
                           std::to_string(rms((int)c[0], (int)c[1]) - ref) + " dB, table " + std::to_string(c[2]));
            expect(rms(-7, 127) - ref < -100, "level velocity -7 at 127 is silent");
        }
        // the output highpass of Extended sounds: first order at 10.3 Hz (-0.41 dB at 32.7 Hz;
        // Dexed's DC filter, about 20 Hz, takes 1.4 dB there)
        {
            auto peak = [&](int key) {
                const auto x = renderExtended({}, key, 1.0, 1.2, 1.2);
                float m = 0;
                for (size_t i = 24000; i < 48000; i++)
                    m = std::max(m, std::fabs(x[i]));
                return 20 * std::log10(m);
            };
            const double drop = peak(24) - peak(60);
            expect(std::fabs(drop + 0.41) < 0.1, "output highpass at note 24: " + std::to_string(drop) + " dB");
        }
        {
            Host h(entry);
            h.activate(48000, 256);
            EventList in;
            in.param(0, 5, 1);
            in.param(0, 235, 0);
            in.note(CLAP_EVENT_NOTE_ON, 10, 9, 0, 60);
            h.process(256, in);
            const auto e = ends(h);
            expect(e.size() == 1 && e[0].id == 9 && e[0].time == 10, "a note with no velocity left ends at once");
        }
        // signed pitch bend ranges: the pitch at full bend up and full bend down
        for (const auto &b : std::vector<std::array<int, 3>>{ { 12, -2, 0x3fff }, { 2, 5, 0 }, { -7, -2, 0x3fff }, { 2, -2, 0x2000 } }) {
            Host h(entry);
            h.activate(rate, 512);
            EventList in;
            in.param(0, 5, 1);
            in.param(0, 40, 1);
            in.param(0, 1051, 99);
            in.param(0, 253, b[0]);
            in.param(0, 254, b[1]);
            const uint8_t bend[3] = { 0xe0, (uint8_t)(b[2] & 0x7f), (uint8_t)(b[2] >> 7) };
            in.midi(0, bend, 3);
            in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, 69, 1.0);
            std::vector<float> x;
            for (int k = 0; k < 60; k++) {
                h.process(512, in);
                in.clear();
                x.insert(x.end(), h.left.begin(), h.left.begin() + 512);
            }
            const double semis = b[2] == 0x3fff ? b[0] : b[2] == 0 ? b[1] : 0.0;
            const double cents = 1200 * std::log2(zeroCrossingHz(x, 4800, x.size()) / f69);
            expect(std::fabs(cents - semis * 100) < 1.0, "pitch bend " + std::to_string(b[0]) + "/" + std::to_string(b[1]) +
                                                              " at " + std::to_string(b[2]) + ": " + std::to_string(cents) + " cents");
        }
        // the bend's steps on the reference's grid, from its renders (`m14b`, `m10b`: range, bend
        // from the centre, steps of 1/2048 octave played at key 69), to a fiftieth of a cent
        for (const auto &b : std::vector<std::array<int, 3>>{ { 2, 40, 0 }, { 2, 44, 1 }, { 2, 84, 3 }, { 2, 124, 5 }, { 2, -44, -1 },
                                                              { 2, -124, -5 }, { 2, 8191, 341 }, { 12, 6, 0 }, { 12, 8, 1 }, { 12, 14, 3 },
                                                              { 12, 28, 6 }, { 7, 8191, 1194 }, { 7, -8192, -853 }, { 7, -4096, -426 } }) {
            Host h(entry);
            h.activate(rate, 512);
            EventList in;
            in.param(0, 5, 1);
            in.param(0, 40, 1);
            in.param(0, 1051, 99);
            in.param(0, 253, b[0]);
            in.param(0, 254, b[0] == 7 ? -5 : -b[0]);
            const int v = 8192 + b[1];
            const uint8_t bend[3] = { 0xe0, (uint8_t)(v & 0x7f), (uint8_t)(v >> 7) };
            in.midi(0, bend, 3);
            in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, 69, 1.0);
            std::vector<float> x;
            for (int k = 0; k < 100; k++) {
                h.process(512, in);
                in.clear();
                x.insert(x.end(), h.left.begin(), h.left.begin() + 512);
            }
            const double cents = 1200 * std::log2(zeroCrossingHz(x, 4800, x.size()) / extStepsHz(13012 + b[2]));
            expect(std::fabs(cents) < 0.02, "pitch bend " + std::to_string(b[1]) + " at range " + std::to_string(b[0]) + ": " +
                                                std::to_string(cents) + " cents from " + std::to_string(b[2]) + " steps");
        }
        // note shift and bend past the reference's ranges (Jacob: no ceiling, -127 to +127): a key
        // shifted past 0-127 plays the rest as semitones (key -20 at ratio 31; key 139 at ratio 0.5,
        // at 96 kHz), a bend of 100 semitones down at ratio 31
        {
            auto renderBent = [&](const std::vector<std::pair<uint32_t, double>> &p, int key, double r, int bend, double seconds) {
                Host h(entry);
                h.activate(r, 512);
                EventList in;
                in.param(0, 5, 1);
                in.param(0, 40, 1);
                in.param(0, 1051, 99);
                for (const auto &q : p)
                    in.param(0, q.first, q.second);
                const uint8_t pb[3] = { 0xe0, (uint8_t)(bend & 0x7f), (uint8_t)(bend >> 7) };
                in.midi(0, pb, 3);
                in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, (int16_t)key, 1.0);
                std::vector<float> x;
                for (size_t done = 0; done < (size_t)(seconds * r); done += 512) {
                    h.process(512, in);
                    in.clear();
                    x.insert(x.end(), h.left.begin(), h.left.begin() + 512);
                }
                return x;
            };
            auto hzAt = [&](const std::vector<std::pair<uint32_t, double>> &p, int key, double r, int bend) {
                return zeroCrossingHz(renderBent(p, key, r, bend, 1.0), (size_t)(0.1 * r), (size_t)r, r);
            };
            const struct {
                std::vector<std::pair<uint32_t, double>> p;
                int key, bend;
                double rate, hz;
                const char *what;
            } wide[] = {
                { { { 244, -60 }, { 1032, 31 } }, 40, 0x2000, 48000, extStepsHz(noteSteps(-20) + extRatioSteps(extRatio(31, 0))), "note shift -60 at key 40" },
                { { { 244, 24 }, { 1032, 0 } }, 115, 0x2000, 96000, extStepsHz(noteSteps(139) + extRatioSteps(extRatio(0, 0))), "note shift +24 at key 115" },
                { { { 254, -100 }, { 1032, 31 } }, 110, 0, 48000,
                  extStepsHz(noteSteps(110) + extRatioSteps(extRatio(31, 0)) + extBendSteps(-8192, 100)), "bend -100 at key 110" },
            };
            for (const auto &w : wide) {
                const double cents = 1200 * std::log2(hzAt(w.p, w.key, w.rate, w.bend) / w.hz);
                expect(std::fabs(cents) < 0.5, std::string(w.what) + ": " + std::to_string(cents) + " cents");
            }
            // the extremes stay finite: note shift and bend at -127 and +127 at keys 0 and 127, a
            // fixed operator at coarse 31 fine 127, on every wave
            float worst = 0;
            bool finite = true;
            for (int w : { 0, 1, 3, 5 })
                for (int shift : { -127, 127 })
                    for (int key : { 0, 127 })
                        for (int bend : { 0, 0x3fff }) {
                            const auto x = renderBent({ { 244, shift }, { 253, shift }, { 254, -shift }, { 1060, w }, { 1061, 7 },
                                                        { 40, 8 }, { 2051, 99 }, { 2031, 1 }, { 2032, 31 }, { 2033, 127 } },
                                                      key, 48000, bend, 0.15);
                            for (float v : x) {
                                finite = finite && std::isfinite(v);
                                worst = std::max(worst, std::fabs(v));
                            }
                        }
            expect(finite && worst < 4, "note shift, bend and fixed frequency at their extremes: peak " + std::to_string(worst));
        }
        for (const auto &dc : std::vector<std::pair<int, double>>{ { -128, -49.95 }, { -64, -24.65 }, { 0, 0 }, { 1, 0 }, { 64, 24.58 }, { 127, 49.63 } }) {
            const auto x = renderExtended({ { 245, dc.first } }, 69, 1.0, 1.0, 0.6);
            const double cents = 1200 * std::log2(zeroCrossingHz(x, 4800, x.size()) / f69);
            expect(std::fabs(cents - dc.second) < 0.5, "detune " + std::to_string(dc.first) + ": " + std::to_string(cents) + " cents");
        }
        std::vector<double> v;
        for (const auto &p : oscairn_fm::params())
            v.push_back(p.def);
        oscairn_fm::convertToExtended(v);
        v[(size_t)oscairn_fm::indexOf(250)] = 5;
        expect(!oscairn_fm::convertToClassic(v), "a sound with a macro offset stays Extended");
    }
    // -- mono, key assign and portamento (Extended part values 255-260)
    {
        auto semis = [&](const std::vector<float> &x, double t0, double t1) {
            return 12 * std::log2(zeroCrossingHz(x, (size_t)(t0 * rate), (size_t)(t1 * rate)) / noteHz(60));
        };
        auto db = [&](const std::vector<float> &x, double t0, double t1) {
            double e = 0;
            for (size_t i = (size_t)(t0 * rate); i < (size_t)(t1 * rate); i++)
                e += (double)x[i] * x[i];
            return 10 * std::log10(e / ((t1 - t0) * rate) + 1e-30);
        };
        // mono: the last key sounds, and the last one still held when it is released
        const auto m = renderMidiNotes({ { 255, 0 } }, { { 0.0, 60, 100 }, { 0.2, 64, 100 }, { 0.4, 67, 100 }, { 0.6, 67, 0 },
                                                      { 0.8, 64, 0 }, { 1.0, 55, 100 }, { 1.2, 55, 0 } }, 1.4);
        expect(std::fabs(semis(m, 0.25, 0.39) - 4) < 0.02 && std::fabs(semis(m, 0.45, 0.59) - 7) < 0.02 &&
                   std::fabs(semis(m, 0.65, 0.79) - 4) < 0.02 && std::fabs(semis(m, 0.85, 0.99)) < 0.02 &&
                   std::fabs(semis(m, 1.05, 1.19) + 5) < 0.02 && std::fabs(semis(m, 1.25, 1.39)) < 0.02,
               "mono: the last key held sounds");
        // a mono note without portamento starts again (attack 60: from silence); with
        // portamento the envelope carries on
        const std::vector<std::pair<uint32_t, double>> slow = { { 255, 0 }, { 1045, 60 } };
        auto withPorta = slow;
        withPorta.push_back({ 257, 1 });
        withPorta.push_back({ 258, 0 });
        const std::vector<std::array<double, 3>> legato = { { 0.0, 60, 100 }, { 1.0, 67, 100 } };
        const auto re = renderMidiNotes(slow, legato, 1.2), le = renderMidiNotes(withPorta, legato, 1.2);
        expect(db(re, 1.02, 1.04) < db(re, 0.95, 0.99) - 20 && std::fabs(db(le, 1.02, 1.04) - db(le, 0.95, 0.99)) < 1,
               "mono: a new note starts again without portamento, carries on with it");
        // the old note fades out over 13.3 ms, its gain the square of the time left's share, while
        // the new one starts (attack 60: still near silent): one period of note 60, 2.7-6.5 ms in
        {
            const double a = 0.0027, b = 0.0065, T = ExtNote::kRetriggerFade;
            double want = 0;
            for (int i = 0; i < 1000; i++) {
                const double t = a + (b - a) * (i + 0.5) / 1000;
                want += std::pow(1 - t / T, 4) / 1000;
            }
            const double got = db(re, 1.0 + a, 1.0 + b) - db(re, 1.0 - 0.0038, 1.0), wantDb = 10 * std::log10(want);
            expect(std::fabs(got - wantDb) < 1 && db(re, 1.0 + T + 0.001, 1.0 + T + 0.005) < db(re, 0.95, 0.99) - 40,
                   "mono retrigger fade: " + std::to_string(got) + " dB 2.7-6.5 ms in, want " + std::to_string(wantDb));
        }
        // portamento rate 1 at time 64 (36.63 semitones per second), fingered: legato only
        const std::vector<std::pair<uint32_t, double>> porta = { { 255, 0 }, { 257, 1 }, { 258, 64 }, { 259, 0 } };
        const auto g = renderMidiNotes(porta, { { 0.0, 48, 100 }, { 0.5, 60, 100 }, { 0.6, 48, 0 } }, 1.2);
        const double mid = semis(g, 0.59, 0.61) + 12; // 0.1 s into the glide
        // (the glide moves in steps about every 9.977 ms, on the engine's clock: 0.2 s holds 20 or 21
        // steps, read between two points 0.2 s apart)
        const double later = semis(g, 0.79, 0.81) + 12;
        expect(std::fabs((later - mid) / 0.2 - 36.63) < 0.5 && std::fabs(semis(g, 0.9, 1.2)) < 0.02,
               "portamento rate at time 64: " + std::to_string((later - mid) / 0.2) + " semitones per second");
        // the steps: one clock for every glide, from the start (prepare), ticking at the start of each
        // 64-sample block in which another 440 samples (at 44.1 kHz) end; the pitch moves by the rate
        // times 9.977 ms at a tick and holds between ticks, in a glide at 0.5 s and one at 1.3 s alike
        {
            const auto st = renderMidiNotes(porta, { { 0.0, 84, 100 }, { 0.5, 96, 100 }, { 0.6, 84, 0 },
                                                     { 1.3, 84, 100 }, { 1.4, 96, 0 } }, 1.6);
            const double scale = rate / 44100.0, T = 440.0 / 44100;
            int checked = 0;
            for (double start : { 0.5, 1.3 }) {
                // the clock's ticks in this glide's first 60 ms, in seconds of the output (which starts
                // after renderMidiNotes' first 512 samples)
                std::vector<double> ticks;
                double clock = 0;
                for (long b = 0; (64.0 * b - 512) / rate < start + 0.06; b++) {
                    clock += 64 / scale;
                    if (clock >= 440) {
                        clock -= 440;
                        if ((64.0 * b - 512) / rate > start + 0.005)
                            ticks.push_back((64.0 * b - 512) / rate);
                    }
                }
                for (double t : ticks) {
                    const double before = semis(st, t - 0.0045, t - 0.0002), after = semis(st, t + 0.0002, t + 0.0041),
                                 later = semis(st, t + 0.0045, t + 0.0085);
                    const double want = start == 0.5 ? 36.63 * T : -36.63 * T;
                    expect(std::fabs(after - before - want) < 0.03 && std::fabs(later - after) < 0.03,
                           "portamento step at " + std::to_string(t) + " s: " + std::to_string(after - before) +
                               " semitones, " + std::to_string(later - after) + " after it");
                    checked++;
                }
            }
            expect(checked >= 8, "portamento steps checked: " + std::to_string(checked));
        }
        const auto f = renderMidiNotes(porta, { { 0.0, 48, 100 }, { 0.4, 48, 0 }, { 0.5, 60, 100 } }, 0.7);
        expect(std::fabs(semis(f, 0.52, 0.6)) < 0.02, "fingered portamento: no glide on a detached note");
        auto fullTime = porta;
        fullTime.push_back({ 259, 1 });
        const auto ft = renderMidiNotes(fullTime, { { 0.0, 48, 100 }, { 0.4, 48, 0 }, { 0.5, 60, 100 } }, 0.7);
        expect(semis(ft, 0.55, 0.57) < -8, "full-time portamento glides from a released note");
        // rate 2: a glide starts at most an octave away
        auto rate2 = porta;
        rate2.push_back({ 260, 2 });
        const auto r2 = renderMidiNotes(rate2, { { 0.0, 36, 100 }, { 0.5, 72, 100 } }, 0.6);
        expect(std::fabs(semis(r2, 0.501, 0.506) - 0) < 0.6, "rate 2: from an octave below, not three");
        expect(std::fabs(extPortamentoRate(64, 1, 24) - 2 * 36.63) < 1e-9 && extPortamentoRate(0, 0, 12) == 0 &&
                   std::fabs(extPortamentoRate(64, 3, 24) - 36.63) < 1e-9 && std::fabs(extPortamentoRate(64, 3, 6) - 36.63 / 2) < 1e-9,
               "portamento time modes");
        // key-on delay (20: 0.52 s) in mono legato with portamento: the legato note takes over at once
        // when the first note sounds, and when the first note still waits it starts when that one would
        {
            const std::vector<std::pair<uint32_t, double>> kd = { { 255, 0 }, { 257, 1 }, { 258, 0 }, { 58, 20 } };
            const auto on = renderMidiNotes(kd, { { 0.0, 60, 100 }, { 1.0, 67, 100 }, { 1.1, 60, 0 } }, 1.5);
            expect(std::fabs(db(on, 1.05, 1.2) - db(on, 0.8, 0.95)) < 1 && std::fabs(semis(on, 1.1, 1.4) - 7) < 0.02,
                   "key-on delay: a legato note takes over a sounding note at once");
            const auto wait = renderMidiNotes(kd, { { 0.0, 60, 100 }, { 0.2, 67, 100 }, { 0.3, 60, 0 } }, 1.2);
            const double start = extKeyOnDelayMs(20) / 1000;
            expect(db(wait, start - 0.06, start - 0.01) < -100 && db(wait, start + 0.05, start + 0.15) > -40 &&
                       std::fabs(semis(wait, start + 0.05, start + 0.3) - 7) < 0.02,
                   "key-on delay: a legato note during the delay starts when the first would have");
        }
        // a key pressed twice: the first note-off releases nothing, the second releases it
        for (int assign : { 0, 1 }) {
            const auto t = renderMidiNotes({ { 256, assign } }, { { 0.0, 60, 100 }, { 0.2, 60, 100 }, { 0.4, 60, 0 }, { 0.6, 60, 0 } }, 0.8);
            expect(db(t, 0.45, 0.55) > -40 && db(t, 0.65, 0.75) < db(t, 0.45, 0.55) - 15,
                   std::string(assign ? "multi" : "single") + ": a key pressed twice needs two note-offs");
        }
        // single: the key's sounding note fades out and starts again; multi: a second voice
        const std::vector<std::array<double, 3>> twice = { { 0.0, 60, 100 }, { 0.5, 60, 100 } };
        const auto sg = renderMidiNotes({ { 256, 0 }, { 1045, 60 } }, twice, 0.6);
        const auto mu = renderMidiNotes({ { 256, 1 }, { 1045, 60 } }, twice, 0.6);
        expect(db(sg, 0.515, 0.525) < db(sg, 0.45, 0.49) - 20 && db(mu, 0.515, 0.525) > db(mu, 0.45, 0.49) - 1,
               "single: a key played again starts again; multi: it adds a voice");
    }
    {
        // A carrier's release (hold) level below the audible range (1-11 play as silence) ends the
        // note once the release reaches it: NOTE_END, and the voice is free again.
        Host h(entry);
        h.activate(48000, 512);
        EventList in;
        in.events.reserve(512);
        in.param(0, 5, 1);
        in.param(0, 40, 1);
        for (int n = 1; n <= 8; n++)
            in.param(0, 1000 * n + 51, n == 1 ? 99 : 0);
        in.param(0, 1044, 1); // operator 1: release level 1, release time 0
        in.param(0, 1048, 0);
        h.process(512, in);
        in.clear();
        in.note(CLAP_EVENT_NOTE_ON, 0, 3, 0, 60);
        h.process(512, in);
        in.clear();
        in.note(CLAP_EVENT_NOTE_OFF, 0, 3, 0, 60);
        bool ended = false;
        for (int b = 0; b < 200 && !ended; b++) {
            h.output.clear();
            h.process(512, in);
            in.clear();
            for (auto e : ends(h))
                ended = ended || e.id == 3;
        }
        expect(ended, "a note whose carrier holds a silent release level ends");
    }
    {
        // The dual types' second section stays stable where its F (the first's times the
        // distance ratio) is still below 1: at 3 kHz with distance +20 its damping had to be
        // limited as the first section's is.
        int cutoff = 0;
        while (cutoff < 1023 && extFilterCutoffHz(cutoff) < 3000)
            cutoff++;
        const auto x = renderExtended({ { 60, extfilter::DualLpf }, { 61, cutoff }, { 63, 0 }, { 66, 20 }, { 1048, 0 } }, 60, 1.0, 0.1, 1.5);
        float late = 0;
        for (size_t i = 48000; i < x.size(); i++)
            late = std::max(late, std::fabs(x[i]));
        expect(late < 1e-4f, "a dual filter below F = 1 decays after the note (largest sample after 1 s: " + std::to_string(late) + ")");
    }
    std::printf("Extended profile: envelope, pitch and LFO rates against the tables, init sound, conversion %s\n",
                failures ? "FAILED" : "pass");
    return failures == 0 ? 0 : 1;
}

// ---- the Extended profile at other sample rates ----

// The reference was measured at 44.1 kHz; Octet runs at any rate. The same Extended cases at 44.1,
// 48, 88.2, 96 and 192 kHz must agree on times (2 %), LFO rates (1 %), pitch (1 cent; 0.05 cent for the
// tuning) and levels (0.5 dB; the filter 0.2 dB), the tolerances the reference comparisons use.
int crossRateTest() {
    int failures = 0;
    using P = std::vector<std::pair<uint32_t, double>>;
    struct Measure {
        const char *what;
        double tolerance;
        bool relative; // a share of the 44.1 kHz value, else an absolute difference
        std::function<double(double)> at;
    };
    // windowed peak level in dB, 1 ms windows, and the first window (in s) meeting a condition
    auto peaks = [](const std::vector<float> &x, double rate) {
        const size_t w = (size_t)(rate / 1000);
        std::vector<double> db;
        for (size_t i = 0; i + w <= x.size(); i += w) {
            float m = 0;
            for (size_t j = i; j < i + w; j++)
                m = std::max(m, std::fabs(x[j]));
            db.push_back(20 * std::log10(std::max(m, 1e-9f)));
        }
        return db;
    };
    auto rmsDb = [](const std::vector<float> &x, double rate, double t0, double t1) {
        double e = 0;
        const size_t a = (size_t)(t0 * rate), b = (size_t)(t1 * rate);
        for (size_t i = a; i < b; i++)
            e += (double)x[i] * x[i];
        return 10 * std::log10(e / (double)(b - a) + 1e-30);
    };
    // the rate of a periodic level change: rising crossings of the level's mean, in Hz
    auto modulationHz = [&](const std::vector<float> &x, double rate, double t0, double t1) {
        const auto db = peaks(x, rate);
        const size_t a = (size_t)(t0 * 1000), b = std::min(db.size(), (size_t)(t1 * 1000));
        double mean = 0;
        for (size_t i = a; i < b; i++)
            mean += db[i];
        mean /= (double)(b - a);
        double first = -1, last = -1;
        int n = 0;
        for (size_t i = a + 1; i < b; i++)
            if (db[i - 1] < mean && db[i] >= mean) {
                const double t = (double)(i - 1) + (mean - db[i - 1]) / (db[i] - db[i - 1]);
                if (first < 0)
                    first = t;
                else
                    n++;
                last = t;
            }
        return n > 0 ? n * 1000.0 / (last - first) : 0.0;
    };
    auto cents = [](double hz, double ref) { return 1200 * std::log2(hz / ref); };
    const P hpf = { { 60, 7 }, { 61, 512 }, { 63, 0 }, { 85, 0 } };
    // the filter EG's probe: a sine at note 84 through HPF12 at cutoff 628, near its frequency
    const P feg = { { 60, 7 }, { 61, 628 }, { 63, 0 }, { 85, 0 } };
    std::vector<Measure> m = {
        { "level of a full carrier (dB)", 0.5, false,
          [&](double r) { return rmsDb(renderExtended({}, 69, 1.0, 1.0, 1.0, nullptr, r), r, 0.2, 0.9); } },
        { "pitch at key 64 (cents)", 0.05, false,
          [&](double r) { return cents(zeroCrossingHz(renderExtended({}, 64, 1.0, 1.0, 1.0, nullptr, r), (size_t)(0.1 * r), (size_t)(0.95 * r), r), 329.6276); } },
        { "pitch of fixed coarse 16 (cents)", 0.05, false,
          [&](double r) { return cents(zeroCrossingHz(renderExtended({ { 1031, 1 }, { 1032, 16 } }, 60, 1.0, 1.0, 1.0, nullptr, r), (size_t)(0.1 * r), (size_t)(0.95 * r), r), 440.0); } },
        { "decay 1 at time 60 to -20 dB (s)", 0.02, true,
          [&](double r) {
              const auto db = peaks(renderExtended({ { 1046, 60 }, { 1042, 0 }, { 1043, 0 } }, 69, 1.0, 3.0, 3.0, nullptr, r), r);
              size_t i = 5;
              while (i < db.size() && db[i] > db[5] - 20)
                  i++;
              return i / 1000.0;
          } },
        { "attack at time 50 to -1 dB (s)", 0.02, true,
          [&](double r) {
              const auto db = peaks(renderExtended({ { 1045, 50 } }, 69, 1.0, 2.0, 2.0, nullptr, r), r);
              const double top = db[1900];
              size_t i = 0;
              while (i < db.size() && db[i] < top - 1)
                  i++;
              return i / 1000.0;
          } },
        { "hold at time 50 (s)", 0.02, true,
          [&](double r) {
              const auto db = peaks(renderExtended({ { 1049, 50 }, { 1044, 50 } }, 69, 1.0, 1.0, 1.0, nullptr, r), r);
              const double top = db[900];
              size_t i = 0;
              while (i < db.size() && db[i] < top - 6)
                  i++;
              return i / 1000.0;
          } },
        { "2nd LFO amplitude rate at speed 30 (Hz)", 0.01, true,
          [&](double r) {
              return modulationHz(renderExtended({ { 42, 1 }, { 43, 30 }, { 46, 1 }, { 48, 99 }, { 1059, 7 } }, 69, 1.0, 6.0, 6.0, nullptr, r), r, 0.5, 5.9);
          } },
        { "2nd LFO delay 60: first dip of 3 dB (s)", 0.02, true,
          [&](double r) {
              const auto db = peaks(renderExtended({ { 42, 1 }, { 43, 40 }, { 45, 60 }, { 46, 1 }, { 48, 99 }, { 1059, 7 } }, 69, 1.0, 4.0, 4.0, nullptr, r), r);
              const double top = db[20];
              size_t i = 20;
              while (i < db.size() && db[i] > top - 3)
                  i++;
              return i / 1000.0;
          } },
        { "part LFO rate at speed 24 (Hz)", 0.01, true,
          [&](double r) {
              P p = feg;
              p.insert(p.end(), { { 210, ext::PartLfoCutoff }, { 211, 8 }, { 201, 6 }, { 202, 24 } });
              return modulationHz(renderExtended(p, 84, 1.0, 6.0, 6.0, nullptr, r), r, 0.5, 5.9);
          } },
        { "key-on delay 20 (s)", 0.02, true,
          [&](double r) {
              const auto x = renderExtended({ { 58, 20 } }, 69, 1.0, 1.5, 1.0, nullptr, r);
              size_t i = 0;
              while (i < x.size() && x[i] == 0.0f)
                  i++;
              return i / r;
          } },
        { "HPF12 at cutoff 512 on note 36 against thru (dB)", 0.2, false,
          [&](double r) {
              return rmsDb(renderExtended(hpf, 36, 1.0, 1.0, 1.0, nullptr, r), r, 0.3, 0.9) -
                     rmsDb(renderExtended({}, 36, 1.0, 1.0, 1.0, nullptr, r), r, 0.3, 0.9);
          } },
        { "LPF24D at cutoff 300 resonance 60 on the feedback tone against thru (dB)", 0.2, false,
          [&](double r) {
              return rmsDb(renderExtended({ { 41, 7 }, { 60, 0 }, { 61, 300 }, { 63, 60 }, { 85, 0 } }, 36, 1.0, 1.0, 1.0, nullptr, r), r, 0.3, 0.9) -
                     rmsDb(renderExtended({ { 41, 7 } }, 36, 1.0, 1.0, 1.0, nullptr, r), r, 0.3, 0.9);
          } },
        { "FEG decay 1 at time 64 from +1200 cents, settled within 0.5 dB (s)", 0.02, true,
          [&](double r) {
              P p = feg;
              p.insert(p.end(), { { 78, 63 }, { 74, 16 }, { 70, 64 } });
              const auto x = renderExtended(p, 84, 1.0, 1.5, 1.5, nullptr, r);
              // (the settled level read before the note-off)
              const double settled = rmsDb(x, r, 1.2, 1.4);
              double t = 0;
              while (t < 1.4 && std::fabs(rmsDb(x, r, t, t + 0.02) - settled) > 0.5)
                  t += 0.005;
              return t;
          } },
        { "portamento at time 64 (semitones per second)", 0.02, true,
          [&](double r) {
              const auto g = renderMidiNotes({ { 255, 0 }, { 257, 1 }, { 258, 64 }, { 259, 0 } },
                                             { { 0.0, 48, 100 }, { 0.5, 60, 100 }, { 0.6, 48, 0 } }, 1.2, r);
              auto semis = [&](double t0, double t1) {
                  return 12 * std::log2(zeroCrossingHz(g, (size_t)(t0 * r), (size_t)(t1 * r), r) / 261.6256);
              };
              return (semis(0.79, 0.81) - semis(0.59, 0.61)) / 0.2;
          } },
    };
    for (const auto &c : m) {
        const double base = c.at(44100);
        for (double r : { 48000.0, 88200.0, 96000.0, 192000.0 }) {
            const double v = c.at(r);
            const double diff = c.relative ? v / base - 1 : v - base;
            const bool ok = std::isfinite(v) && std::fabs(diff) <= c.tolerance;
            std::printf("%-70s 44.1 kHz %10.4f  %3.0f kHz %10.4f  %s\n", c.what, base, r / 1000, v, ok ? "ok" : "NOT MET");
            if (!ok)
                failures++;
        }
    }
    std::printf("Extended profile at 44.1, 48, 88.2, 96 and 192 kHz %s\n", failures ? "FAILED" : "pass");
    return failures ? 1 : 0;
}

// ---- renders for comparing with reference recordings ----

// render <out.wav> <rate> <note> <velocity 1-127> <hold s> <tail s> [id=value ...]: one note
// with the given parameter values, as a mono 32-bit float WAV (the note starts at frame 0).
int renderToWav(const std::vector<char *> &args, bool stereo) {
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
    out.reserve((total + 512) * 2);
    in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, (int16_t)key, velocity);
    for (size_t done = 0; done < total; done += 512) {
        if (holdFrames >= done && holdFrames < done + 512)
            in.note(CLAP_EVENT_NOTE_OFF, (uint32_t)(holdFrames - done), -1, 0, (int16_t)key, 0);
        h.process(512, in);
        in.clear();
        for (size_t i = 0; i < 512; i++) {
            out.push_back(h.left[i]);
            if (stereo)
                out.push_back(h.right[i]);
        }
    }
    const uint16_t channels = stereo ? 2 : 1;
    out.resize(total * channels);
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char *)&v, 2); };
    const uint32_t bytes = (uint32_t)(out.size() * 4);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(3); u16(channels);
    u32((uint32_t)rate); u32((uint32_t)rate * 4 * channels); u16(4 * channels); u16(32); f.write("data", 4); u32(bytes);
    f.write((const char *)out.data(), bytes);
    return f ? 0 : 1;
}

// render-seq <out.wav> <rate> <length s> [event ...] [id=value ...]: a sequence on channel 1 as a
// stereo 32-bit float WAV. Events (t in seconds): on@t:key:velocity off@t:key cc@t:number:value
// pb@t:value (0-16383). The notes are MIDI events, as the reference recorder sends them.
int renderSeqToWav(const std::vector<char *> &args) {
    if (args.size() < 5)
        return 2;
    const char *path = args[2];
    const double rate = std::atof(args[3]);
    const size_t total = (size_t)std::llround(std::atof(args[4]) * rate);
    struct Ev { size_t frame; uint8_t b[3]; };
    std::vector<Ev> evs;
    Host h(entry);
    h.activate(rate, 512);
    EventList in;
    in.events.reserve(1024);
    for (size_t i = 5; i < args.size(); i++) {
        const char *at = std::strchr(args[i], '@');
        if (at != nullptr) {
            const std::string kind(args[i], (size_t)(at - args[i]));
            double t = 0;
            int a = 0, b = 0, n = 0;
            if (kind == "on" && std::sscanf(at + 1, "%lf:%d:%d%n", &t, &a, &b, &n) == 3)
                evs.push_back({ (size_t)std::llround(t * rate), { 0x90, (uint8_t)a, (uint8_t)b } });
            else if (kind == "off" && std::sscanf(at + 1, "%lf:%d%n", &t, &a, &n) == 2)
                evs.push_back({ (size_t)std::llround(t * rate), { 0x80, (uint8_t)a, 0 } });
            else if (kind == "cc" && std::sscanf(at + 1, "%lf:%d:%d%n", &t, &a, &b, &n) == 3)
                evs.push_back({ (size_t)std::llround(t * rate), { 0xb0, (uint8_t)a, (uint8_t)b } });
            else if (kind == "pb" && std::sscanf(at + 1, "%lf:%d%n", &t, &a, &n) == 2)
                evs.push_back({ (size_t)std::llround(t * rate), { 0xe0, (uint8_t)(a & 127), (uint8_t)((a >> 7) & 127) } });
            else
                return 2;
            continue;
        }
        const char *eq = std::strchr(args[i], '=');
        if (eq == nullptr)
            return 2;
        in.param(0, (clap_id)std::atol(args[i]), std::atof(eq + 1));
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev &x, const Ev &y) { return x.frame < y.frame; });
    h.process(512, in);
    in.clear();
    std::vector<float> out;
    out.reserve((total + 512) * 2);
    size_t next = 0;
    for (size_t done = 0; done < total; done += 512) {
        for (; next < evs.size() && evs[next].frame < done + 512; next++)
            in.midi((uint32_t)(evs[next].frame - done), evs[next].b, 3);
        h.process(512, in);
        in.clear();
        for (size_t i = 0; i < 512; i++) {
            out.push_back(h.left[i]);
            out.push_back(h.right[i]);
        }
    }
    out.resize(total * 2);
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char *)&v, 2); };
    const uint32_t bytes = (uint32_t)(out.size() * 4);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(3); u16(2);
    u32((uint32_t)rate); u32((uint32_t)rate * 8); u16(8); u16(32); f.write("data", 4); u32(bytes);
    f.write((const char *)out.data(), bytes);
    return f ? 0 : 1;
}

// ---- CPU benchmark (informational, not a gate: the plan defines no CPU budget) ----

// bench [seconds]: held notes of representative sounds at 48 kHz in 64-sample blocks, 16 voices
// (the engine's polyphony) per instance and as many instances as the voice count needs, all on
// this thread. Prints the real-time factor (render time over audio time) at 1, 16 and 64 voices
// and the most voices measured to fit within 100 % and 50 % of one core.
int benchmark(const std::vector<char *> &args) {
    const double seconds = args.size() > 2 ? std::atof(args[2]) : 0.5;
    constexpr double kRate = 48000;
    constexpr uint32_t kBlock = 64;
    using Params = std::vector<std::pair<clap_id, double>>;
    // Classic: algorithm 1 with every operator sounding, feedback 6, LFO pitch modulation
    Params classic = { { 1, 1 }, { 2, 6 }, { 12, 10 } };
    for (int n = 2; n <= 6; n++)
        classic.push_back({ (clap_id)(1000 * n + 17), 80 });
    // Extended, 8 sine operators: algorithm 1, feedback 5, the 2nd LFO per voice on pitch and amplitude
    Params sines = { { 5, 1 }, { 40, 1 }, { 41, 5 }, { 43, 60 }, { 46, 1 }, { 47, 20 }, { 48, 20 } };
    for (int n = 1; n <= 8; n++)
        sines.push_back({ (clap_id)(1000 * n + 51), 85 });
    // Extended with waves (formant 2 spread 5, odd harmonics, all harmonics spread 7, formant 1),
    // the texture macro, and a 24 dB lowpass whose cutoff the filter EG moves
    Params waves = sines;
    const int wave[8][3] = { { 6, 5, 20 }, { 3, 3, 0 }, { 1, 7, 0 }, { 5, 3, 9 }, { 4, 2, 0 }, { 2, 4, 0 }, { 6, 7, 40 }, { 0, 0, 0 } };
    for (int n = 1; n <= 8; n++)
        for (int k = 0; k < 3; k++)
            waves.push_back({ (clap_id)(1000 * n + 60 + k), (double)wave[n - 1][k] });
    for (auto p : Params { { 252, 20 }, { 60, 0 }, { 61, 500 }, { 63, 60 }, { 69, 70 }, { 70, 80 }, { 74, 127 }, { 75, -60 } })
        waves.push_back(p);
    const std::pair<const char *, const Params *> sounds[] = { { "Classic, 6 operators", &classic },
                                                              { "Extended, 8 sines, 2nd LFO", &sines },
                                                              { "Extended, waves, texture, filter", &waves } };
    // render time per second of audio for `voices` held notes of one sound
    auto rtf = [&](const Params &p, int voices) {
        std::vector<std::unique_ptr<Host>> hosts;
        EventList in;
        in.events.reserve(256);
        for (int v = 0; v < voices; v += 16) {
            hosts.push_back(std::make_unique<Host>(entry));
            Host &h = *hosts.back();
            h.activate(kRate, kBlock);
            for (auto &[id, value] : p)
                in.param(0, id, value);
            h.process(kBlock, in);
            in.clear();
            for (int k = 0; k < std::min(16, voices - v); k++)
                in.note(CLAP_EVENT_NOTE_ON, 0, -1, 0, (int16_t)(40 + 2 * k), 0.8);
            h.process(kBlock, in);
            in.clear();
        }
        const int blocks = (int)(seconds * kRate / kBlock);
        for (int b = 0; b < blocks / 4; b++) // past the attacks
            for (auto &h : hosts)
                h->process(kBlock, in);
        float peak = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; b++)
            for (auto &h : hosts) {
                h->process(kBlock, in);
                peak = std::max(peak, std::fabs(h->left[0]));
            }
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(peak > 0 && std::isfinite(peak), "the benchmark's notes sound");
        return took / (blocks * kBlock / kRate);
    };
    std::printf("48 kHz, %u-sample blocks, %.2f s of audio per measurement, one thread\n", kBlock, seconds);
    for (auto &[name, p] : sounds) {
        const double r1 = rtf(*p, 1), r16 = rtf(*p, 16), r64 = rtf(*p, 64);
        int fit100 = 0, fit50 = 0;
        for (int voices = 16; voices <= 2048 && fit100 == voices - 16; voices += 16) {
            const double r = rtf(*p, voices);
            if (r < 1.0)
                fit100 = voices;
            if (r < 0.5)
                fit50 = voices;
        }
        std::printf("%-34s RTF 1 voice %.5f, 16 voices %.4f, 64 voices %.4f (%.2f us per voice per block); "
                    "fits within one core: %d voices, half a core: %d voices\n",
                    name, r1, r16, r64, r64 / 64 * kBlock / kRate * 1e6, fit100, fit50);
    }
    return 0;
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

// ---- snapshots prepared beside the playing sounds (oscairn.sound-snapshots) ----

std::string saveSnapshot(Instrument &t) {
    std::string saved;
    clap_ostream_t os { &saved, [](const clap_ostream_t *st, const void *buf, uint64_t size) -> int64_t {
                           static_cast<std::string *>(st->ctx)->append(static_cast<const char *>(buf), size);
                           return (int64_t)size;
                       } };
    check(t.ctx->state->save(t.host.plugin, &os), "snapshot save");
    return saved;
}

struct SnapshotReader {
    const std::string *data;
    size_t pos = 0;
    clap_istream_t stream { this, [](const clap_istream_t *st, void *buf, uint64_t size) -> int64_t {
                               auto *r = static_cast<SnapshotReader *>(st->ctx);
                               const size_t n = std::min<size_t>(size, r->data->size() - r->pos);
                               std::memcpy(buf, r->data->data() + r->pos, n);
                               r->pos += n;
                               return (int64_t)n;
                           } };
    explicit SnapshotReader(const std::string &d) : data(&d) {}
};

bool loadSnapshot(Instrument &t, const std::string &snapshot) {
    SnapshotReader r(snapshot);
    return t.ctx->state->load(t.host.plugin, &r.stream);
}

bool prepareSnapshot(const clap_plugin_t *plugin, const std::string &snapshot, uint32_t *selected, char *message = nullptr, uint32_t capacity = 0) {
    auto *x = static_cast<const oscairn_plugin_sound_snapshots_t *>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
    SnapshotReader r(snapshot);
    return x != nullptr && x->prepare(plugin, &r.stream, selected, message, capacity);
}

uint32_t preparedCount(Instrument &t) {
    oscairn_sound_context_info_t i;
    uint32_t n = 0;
    for (uint32_t k = 0; k < t.ctx->capacity(t.host.plugin); k++)
        n += t.ctx->get(t.host.plugin, k, &i);
    return n;
}

// Selects a prepared snapshot's sounds as the host does at the cue.
void cue(Instrument &t, const uint32_t *selected) {
    if (std::all_of(selected, selected + 16, [&](uint32_t id) { return id == selected[0]; }))
        check(t.select(selected[0]), "select at the cue");
    else
        for (int16_t ch = 0; ch < 16; ch++)
            check(t.select(selected[ch], ch), "select at the cue");
}

int snapshotsTest() {
    {
        Instrument t;
        auto *x = static_cast<const oscairn_plugin_sound_snapshots_t *>(t.host.plugin->get_extension(t.host.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        check(x != nullptr && x->prepare != nullptr, "oscairn.sound-snapshots offered");
    }
    // S: a brighter Default (all channels but 2) and sound B on channel 2.
    std::string S;
    {
        Instrument s;
        s.in.param(0, 1017, 70);
        s.in.param(0, 1001, 40);
        s.run();
        check(s.select(s.prepare("B", soundB), 1), "S's second sound");
        s.run();
        S = saveSnapshot(s);
    }
    auto play = [](Instrument &t) {
        std::vector<float> r;
        t.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 60);
        t.in.note(CLAP_EVENT_NOTE_ON, 5, 2, 1, 67);
        for (int k = 0; k < 40; k++) {
            if (k == 20)
                t.in.note(CLAP_EVENT_NOTE_OFF, 30, 1, 0, 60);
            const auto &o = t.run();
            r.insert(r.end(), o.begin(), o.end());
        }
        return r;
    };
    {
        // Prepared beside the default sound and selected, S plays as S loaded.
        Instrument loaded, beside;
        check(loadSnapshot(loaded, S), "S loads");
        loaded.run();
        beside.run();
        uint32_t selected[16];
        const auto t0 = std::chrono::steady_clock::now();
        check(prepareSnapshot(beside.host.plugin, S, selected), "S prepared beside");
        const double prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(preparedCount(beside) == 3, "two sounds added");
        check(selected[0] != 0 && selected[1] != 0 && selected[0] != selected[1], "fresh IDs, one per sound");
        for (int ch = 2; ch < 16; ch++)
            check(selected[ch] == selected[0], "the Default's channels");
        check(beside.info(selected[0]).selected_channels == 0 && beside.info(selected[1]).selected_channels == 0, "nothing selected");
        cue(beside, selected);
        // the cue's block, against a block that changes nothing (median of 25 each)
        std::vector<double> cueUs, plainUs;
        for (int k = 0; k < 25; k++) {
            Instrument a, b;
            uint32_t sel[16];
            check(prepareSnapshot(a.host.plugin, S, sel), "S prepared beside");
            a.run();
            b.run();
            cue(a, sel);
            for (auto [t, us] : { std::pair{ &a, &cueUs }, std::pair{ &b, &plainUs } }) {
                const auto t1 = std::chrono::steady_clock::now();
                t->run();
                us->push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count());
            }
        }
        std::sort(cueUs.begin(), cueUs.end());
        std::sort(plainUs.begin(), plainUs.end());
        beside.run();
        loaded.run();
        const auto a = play(loaded), b = play(beside);
        check(a == b && audible(a), "a snapshot prepared beside and selected plays as the snapshot loaded");
        std::printf("2 sounds prepared beside in %.3f ms; the cue's 512-frame block %.1f us, a block without one %.1f us (medians)\n", prepareMs,
                    cueUs[12], plainUs[12]);
    }
    {
        // A note held on the outgoing sound rings out beside the incoming one, which plays as alone.
        Instrument mixed, old, fresh;
        const std::vector<oscairn_sound_parameter_t> slow = { { 1019, 2 }, { 1004, 40 } }; // a release of about a second
        const uint32_t x = mixed.prepare("slow", slow);
        check(mixed.select(x) && old.select(old.prepare("slow", slow)) && loadSnapshot(fresh, S), "outgoing sound");
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 9, 0, 48);
        old.in.note(CLAP_EVENT_NOTE_ON, 0, 9, 0, 48);
        mixed.run();
        old.run();
        fresh.run();
        uint32_t selected[16];
        check(prepareSnapshot(mixed.host.plugin, S, selected), "S prepared beside a held note");
        cue(mixed, selected);
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 10, 0, 72);
        fresh.in.note(CLAP_EVENT_NOTE_ON, 0, 10, 0, 72);
        for (int k = 0; k < 8; k++)
            same(mixed.run(), old.run(), fresh.run(), "the held note keeps the outgoing sound; the new one plays the incoming");
        check(audible(old.out) && audible(fresh.out), "both are heard");
        check(!mixed.discard(x), "a sounding outgoing sound cannot be discarded");
        mixed.in.note(CLAP_EVENT_NOTE_OFF, 0, 9, 0, 48);
        old.in.note(CLAP_EVENT_NOTE_OFF, 0, 9, 0, 48);
        same(mixed.run(), old.run(), fresh.run(), "the outgoing note is released");
        check(mixed.info(x).active_voices == 1 && !mixed.info(x).idle && !mixed.discard(x), "its release rings and pins it");
        int blocks = 1;
        while (!mixed.info(x).idle && blocks < 2000) {
            same(mixed.run(), old.run(), fresh.run(), "the outgoing release rings out");
            blocks++;
        }
        std::printf("the outgoing sound rang out for %.2f s after its note-off\n", blocks * 512 / 48000.0);
        check(mixed.info(x).idle && mixed.discard(x), "then it is discarded");
        check(mixed.info(selected[0]).active_voices == 1, "the incoming sound plays on");
    }
    {
        // With a ring-out buffer (draft 2) the retiring sound plays there, not in the main output:
        // the main output is the incoming sound alone, the buffer the outgoing alone.
        Instrument mixed, old, fresh;
        auto *x = static_cast<const oscairn_plugin_sound_snapshots_t *>(mixed.host.plugin->get_extension(mixed.host.plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        check(x != nullptr && x->ring_out != nullptr, "ring_out offered");
        std::vector<float> ringLeft(512), ringRight(512);
        float *ring[2] = { ringLeft.data(), ringRight.data() };
        clap_audio_buffer_t buffer {};
        buffer.data32 = ring;
        buffer.channel_count = 2;
        auto runRinging = [&]() -> const std::vector<float> & {
            std::fill(ringLeft.begin(), ringLeft.end(), 0.0f);
            std::fill(ringRight.begin(), ringRight.end(), 0.0f);
            x->ring_out(mixed.host.plugin, &buffer);
            return mixed.run();
        };
        const std::vector<oscairn_sound_parameter_t> slow = { { 1019, 2 }, { 1004, 40 } };
        check(mixed.select(mixed.prepare("slow", slow)) && old.select(old.prepare("slow", slow)) && loadSnapshot(fresh, S), "outgoing sound");
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 9, 0, 48);
        old.in.note(CLAP_EVENT_NOTE_ON, 0, 9, 0, 48);
        check(runRinging() == old.run() && audible(old.out), "before the cue the sound plays in the main output");
        check(std::all_of(ringLeft.begin(), ringLeft.end(), [](float v) { return v == 0; }), "and nothing in the ring-out buffer");
        fresh.run();
        uint32_t selected[16];
        check(prepareSnapshot(mixed.host.plugin, S, selected), "S prepared beside a held note");
        cue(mixed, selected);
        mixed.in.note(CLAP_EVENT_NOTE_ON, 0, 10, 0, 72);
        fresh.in.note(CLAP_EVENT_NOTE_ON, 0, 10, 0, 72);
        bool tail = false;
        for (int k = 0; k < 60; k++) {
            if (k == 4) {
                mixed.in.note(CLAP_EVENT_NOTE_OFF, 0, 9, 0, 48);
                old.in.note(CLAP_EVENT_NOTE_OFF, 0, 9, 0, 48);
            }
            check(runRinging() == fresh.run(), "after the cue the main output is the incoming sound alone");
            check(ringLeft == old.run() && ringRight == ringLeft, "and the ring-out buffer the outgoing sound alone");
            tail = tail || (k > 4 && audible(old.out));
        }
        check(tail && audible(fresh.out), "its release rings in the buffer");
    }
    {
        // All or nothing: a bad snapshot, or too few free contexts, adds nothing and says why.
        Instrument full;
        uint32_t selected[16];
        char message[128] = "";
        check(!prepareSnapshot(full.host.plugin, "octet-contexts 1\nnonsense\n", selected, message, sizeof message) && *message, "a bad snapshot is refused");
        std::string missingDefault = S;
        missingDefault.replace(missingDefault.find("context 0 "), 10, "context 7 ");
        check(!prepareSnapshot(full.host.plugin, missingDefault, selected), "a snapshot without its Default is refused");
        std::vector<uint32_t> ids;
        for (int k = 0; k < 6; k++)
            ids.push_back(full.prepare("filler", {}));
        *message = 0;
        check(!prepareSnapshot(full.host.plugin, S, selected, message, sizeof message) && *message && preparedCount(full) == 7,
              "too few free contexts: nothing added");
        std::printf("refused: %s\n", message);
        check(full.discard(ids.back()) && prepareSnapshot(full.host.plugin, S, selected) && preparedCount(full) == 8, "two free contexts are enough");
    }
    std::printf("prepared beside equals loaded, held notes ring out on the outgoing sound, all or nothing\n");
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


// ---- presets: cartridge voices (source/oscairn/Presets.h) ----

// Packs 155-byte voices into a 32-voice bulk dump (F0 43 00 09 20 00 ... checksum F7).
std::vector<uint8_t> cartridge(const std::vector<std::vector<uint8_t>> &voices) {
    std::vector<uint8_t> m = { 0xF0, 0x43, 0x00, 0x09, 0x20, 0x00 };
    for (const auto &v : voices) {
        uint8_t b[128] = {};
        for (int op = 0; op < 6; op++) {
            const uint8_t *s = v.data() + op * 21;
            uint8_t *d = b + op * 17;
            std::memcpy(d, s, 11);
            d[11] = (uint8_t)((s[11] & 3) | ((s[12] & 3) << 2));
            d[12] = (uint8_t)((s[13] & 7) | ((s[20] & 15) << 3));
            d[13] = (uint8_t)((s[14] & 3) | ((s[15] & 7) << 2));
            d[14] = s[16];
            d[15] = (uint8_t)((s[17] & 1) | ((s[18] & 31) << 1));
            d[16] = s[19];
        }
        std::memcpy(b + 102, v.data() + 126, 9);
        b[111] = (uint8_t)((v[135] & 7) | ((v[136] & 1) << 3));
        std::memcpy(b + 112, v.data() + 137, 4);
        b[116] = (uint8_t)((v[141] & 1) | ((v[142] & 7) << 1) | ((v[143] & 7) << 4));
        b[117] = v[144];
        std::memcpy(b + 118, v.data() + 145, 10);
        m.insert(m.end(), b, b + 128);
    }
    int sum = 0;
    for (size_t i = 6; i < m.size(); i++)
        sum += m[i];
    m.push_back((uint8_t)((128 - (sum & 127)) & 127));
    m.push_back(0xF7);
    return m;
}

// The fixture: 32 generated voices (never a factory bank), one with a blank name and one
// with characters the names show differently.
std::vector<std::vector<uint8_t>> fixtureVoices() {
    std::vector<std::vector<uint8_t>> v;
    for (int i = 0; i < 32; i++)
        v.push_back(voice(i));
    std::memset(v[5].data() + 145, ' ', 10);
    std::memcpy(v[6].data() + 145, "A\\B~C\x7f  x ", 10);
    return v;
}

void writeFile(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary);
    f.write((const char *)data.data(), (std::streamsize)data.size());
    check((bool)f, "write " + path);
}

struct Indexer {
    clap_preset_discovery_indexer_t indexer {};
    struct Location { uint32_t flags; std::string name, path; };
    std::vector<Location> locations;
    std::vector<std::string> types;
    Indexer() {
        indexer.clap_version = CLAP_VERSION;
        indexer.name = "dexed tests";
        indexer.indexer_data = this;
        indexer.declare_filetype = [](const clap_preset_discovery_indexer_t *i, const clap_preset_discovery_filetype_t *t) {
            static_cast<Indexer *>(i->indexer_data)->types.push_back(t->file_extension ? t->file_extension : "");
            return true;
        };
        indexer.declare_location = [](const clap_preset_discovery_indexer_t *i, const clap_preset_discovery_location_t *l) {
            static_cast<Indexer *>(i->indexer_data)->locations.push_back({ l->flags, l->name, l->location ? l->location : "" });
            return true;
        };
        indexer.declare_soundpack = [](const clap_preset_discovery_indexer_t *, const clap_preset_discovery_soundpack_t *) { return true; };
        indexer.get_extension = [](const clap_preset_discovery_indexer_t *, const char *) -> const void * { return nullptr; };
    }
};

struct Listed {
    struct Preset { std::string name, key, plugin; };
    std::vector<Preset> presets;
    std::vector<std::string> errors;
    clap_preset_discovery_metadata_receiver_t receiver {};
    static Listed *self(const clap_preset_discovery_metadata_receiver_t *r) { return static_cast<Listed *>(r->receiver_data); }
    Listed() {
        receiver.receiver_data = this;
        receiver.on_error = [](const clap_preset_discovery_metadata_receiver_t *r, int32_t, const char *m) { self(r)->errors.push_back(m); };
        receiver.begin_preset = [](const clap_preset_discovery_metadata_receiver_t *r, const char *name, const char *key) {
            self(r)->presets.push_back({ name ? name : "", key ? key : "", "" });
            return true;
        };
        receiver.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t *r, const clap_universal_plugin_id_t *id) {
            self(r)->presets.back().plugin = std::string(id->abi) + ":" + id->id;
        };
        receiver.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t *, const char *) {};
        receiver.set_flags = [](const clap_preset_discovery_metadata_receiver_t *, uint32_t) {};
        receiver.add_creator = [](const clap_preset_discovery_metadata_receiver_t *, const char *) {};
        receiver.set_description = [](const clap_preset_discovery_metadata_receiver_t *, const char *) {};
        receiver.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t *, clap_timestamp, clap_timestamp) {};
        receiver.add_feature = [](const clap_preset_discovery_metadata_receiver_t *, const char *) {};
        receiver.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t *, const char *, const char *) {};
    }
};

// The malformed cartridges: each is refused with a reason.
std::vector<std::pair<std::string, std::vector<uint8_t>>> malformedCartridges(const std::vector<uint8_t> &good) {
    std::vector<uint8_t> shortDump(good.begin(), good.end() - 100);
    shortDump.back() = 0xF7;
    std::vector<uint8_t> otherFormat = good;
    otherFormat[3] = 0x00;
    return {
        { "empty", {} },
        { "text", { 'n', 'o', 't', ' ', 's', 'y', 's', 'e', 'x' } },
        { "truncated", std::vector<uint8_t>(good.begin(), good.begin() + 2000) },
        { "short-dump", shortDump },
        { "other-format", otherFormat },
        { "raw-voices", std::vector<uint8_t>(good.begin() + 6, good.begin() + 4102) },
    };
}

int presetsTest() {
    const auto voices = fixtureVoices();
    const std::vector<uint8_t> cart = cartridge(voices);
    check(cart.size() == 4104, "fixture size");
    const std::string fixture = "presets-fixture.syx";
    writeFile(fixture, cart);

    // the desktop Dexed's cartridge folder, in a home made for the test
    char cwd[4096];
    check(getcwd(cwd, sizeof cwd) != nullptr, "getcwd");
    const std::string home = std::string(cwd) + "/presets-home";
#ifdef __APPLE__
    const std::string userFolder = home + "/Library/Application Support/DigitalSuburban/Dexed/Cartridges";
#else
    const std::string userFolder = home + "/.local/share/DigitalSuburban/Dexed/Cartridges";
    unsetenv("XDG_DATA_HOME");
#endif
    check(std::system(("mkdir -p '" + userFolder + "'").c_str()) == 0, "make the user cartridge folder");
    setenv("HOME", home.c_str(), 1);

    auto *factory = static_cast<const clap_preset_discovery_factory_t *>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    check(factory != nullptr && factory == entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT), "preset discovery factory");
    check(factory->count(factory) == 1, "one provider");
    const auto *desc = factory->get_descriptor(factory, 0);
    Indexer ix;
    const auto *provider = factory->create(factory, &ix.indexer, desc->id);
    check(provider != nullptr && provider->init(provider), "provider init");

    // locations: the built-in cartridges beside the module (the build puts them there) and the user's folder
    const std::string builtin = modulePath.substr(0, modulePath.find_last_of('/') + 1) + "Octet/Cartridges";
    check(ix.locations.size() == (modulePath.empty() ? 1u : 2u), "two locations");
    for (const auto &l : ix.locations) {
        const bool user = l.path == userFolder;
        check(user || l.path == builtin, "location " + l.path);
        check(l.flags == (user ? CLAP_PRESET_DISCOVERY_IS_USER_CONTENT : CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT), "location flags");
    }
    check(ix.types.size() == ix.locations.size() && std::all_of(ix.types.begin(), ix.types.end(), [](auto &t) { return t == "syx"; }),
          "file type syx");

    // every voice of the fixture, keys 0..31, named from the cartridge
    Listed listed;
    check(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, fixture.c_str(), &listed.receiver), "get_metadata");
    check(listed.errors.empty() && listed.presets.size() == 32, "32 presets");
    for (int i = 0; i < 32; i++) {
        char expected[16];
        std::snprintf(expected, sizeof expected, "GEN %06d", i);
        const std::string name = i == 5 ? "Voice 6" : i == 6 ? "AYB>C<  x" : expected;
        check(listed.presets[i].name == name, "preset name " + listed.presets[i].name);
        check(listed.presets[i].key == std::to_string(i), "load key");
        check(listed.presets[i].plugin == "clap:org.oscairn.fm", "plugin ID");
    }

    // malformed cartridges list nothing and report why
    for (const auto &[name, data] : malformedCartridges(cart)) {
        const std::string path = "presets-" + name + ".syx";
        writeFile(path, data);
        Listed bad;
        check(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), &bad.receiver), "get_metadata " + name);
        check(bad.presets.empty() && bad.errors.size() == 1, "malformed cartridge refused: " + name);
    }
    Listed missing;
    provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "presets-missing.syx", &missing.receiver);
    check(missing.presets.empty() && missing.errors.size() == 1, "a missing file is refused");
    provider->destroy(provider);

    // a context prepared from each voice, and preset-load into the sound shown, render exactly
    // as the voice loaded the existing way (its bytes and switches as parameter events)
    auto play = [](Instrument &x, std::vector<float> &l, std::vector<float> &r) {
        for (int k = 0; k < 100; k++) {
            if (k == 0) {
                x.in.note(CLAP_EVENT_NOTE_ON, 0, 1, 0, 60, 0.8);
                x.in.note(CLAP_EVENT_NOTE_ON, 100, 2, 0, 67, 0.5);
            }
            if (k == 70)
                x.in.note(CLAP_EVENT_NOTE_OFF, 7, -1, -1, -1);
            x.run();
            l.insert(l.end(), x.host.left.begin(), x.host.left.end());
            r.insert(r.end(), x.host.right.begin(), x.host.right.end());
        }
    };
    auto preparePreset = [&](Instrument &x, const char *name, const std::string &file, const char *key, std::string *why = nullptr) {
        char message[256] = {};
        check(x.ctx->prepare_preset != nullptr, "prepare_preset");
        const uint32_t id = x.ctx->prepare_preset(x.host.plugin, name, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file.c_str(), key, message, sizeof message);
        if (why != nullptr)
            *why = message;
        return id;
    };
    for (int i = 0; i < 32; i++) {
        Instrument ref, prepared, loaded;
        for (const auto &p : oscairn_fm::params()) {
            if (p.kind == oscairn_fm::Kind::Byte)
                ref.in.param(0, p.id, (double)voices[i][p.target] - p.offset);
            else if (p.kind == oscairn_fm::Kind::OpSwitch)
                ref.in.param(0, p.id, 1);
        }
        ref.run();
        const std::string key = std::to_string(i);
        const uint32_t id = preparePreset(prepared, "Prepared", fixture, key.c_str());
        check(id != CLAP_INVALID_ID && prepared.select(id), "prepare and select voice " + key);
        prepared.run();
        check(std::string(prepared.info(id).name) == "Prepared", "the context has the name it was given");
        auto *load = static_cast<const clap_plugin_preset_load_t *>(loaded.host.plugin->get_extension(loaded.host.plugin, CLAP_EXT_PRESET_LOAD));
        check(load != nullptr && load->from_location(loaded.host.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, fixture.c_str(), key.c_str()),
              "preset-load voice " + key);
        loaded.run();
        std::vector<float> l0, r0, l1, r1, l2, r2;
        play(ref, l0, r0);
        play(prepared, l1, r1);
        play(loaded, l2, r2);
        // (many generated voices are quiet: deep amplitude modulation)
        check(std::any_of(l0.begin(), l0.end(), [](float v) { return v != 0; }), "voice " + key + " is not silent");
        check(l0 == l1 && r0 == r1, "voice " + key + ": the prepared context renders bit-identically");
        check(l0 == l2 && r0 == r2, "voice " + key + ": preset-load renders bit-identically");
    }

    // a saved Sound still opens after the cartridge is gone: the snapshot holds the voice
    {
        const std::string gone = "presets-gone.syx";
        writeFile(gone, cart);
        Instrument a(false), b(false); // the snapshot is exclusive: saved and loaded while inactive
        const uint32_t id = preparePreset(a, "From a cartridge", gone, "9");
        check(id != CLAP_INVALID_ID && a.select(id), "prepare from the cartridge");
        std::string snapshot;
        clap_ostream_t out { &snapshot, [](const clap_ostream_t *s, const void *buffer, uint64_t size) -> int64_t {
                                static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buffer), size);
                                return (int64_t)size;
                            } };
        check(a.ctx->state->save(a.host.plugin, &out), "snapshot save");
        check(std::remove(gone.c_str()) == 0, "remove the cartridge");
        std::string why;
        check(preparePreset(b, "Gone", gone, "9", &why) == CLAP_INVALID_ID && !why.empty(), "the cartridge is gone");
        struct Reader { const std::string *data; size_t pos; } reader { &snapshot, 0 };
        clap_istream_t in { &reader, [](const clap_istream_t *s, void *buffer, uint64_t size) -> int64_t {
                               auto *r = static_cast<Reader *>(s->ctx);
                               const size_t n = std::min<size_t>(size, r->data->size() - r->pos);
                               std::memcpy(buffer, r->data->data() + r->pos, n);
                               r->pos += n;
                               return (int64_t)n;
                           } };
        check(b.ctx->state->load(b.host.plugin, &in), "snapshot load without the cartridge");
        bool named = false;
        oscairn_sound_context_info_t info {};
        for (uint32_t k = 0; k < b.ctx->capacity(b.host.plugin); k++)
            named = named || (b.ctx->get(b.host.plugin, k, &info) && std::string(info.name) == "From a cartridge" && info.selected_channels == 0xffff);
        check(named, "the reopened sound keeps its name and selection");
        std::vector<float> l0, r0, l1, r1;
        a.host.activate(48000, 512);
        b.host.activate(48000, 512);
        play(a, l0, r0);
        play(b, l1, r1);
        check(audible(l0) && l0 == l1 && r0 == r1, "the reopened sound plays the same");
    }

    // malformed cartridges and bad load keys are refused with a reason, and change nothing
    {
        Instrument x;
        auto *load = static_cast<const clap_plugin_preset_load_t *>(x.host.plugin->get_extension(x.host.plugin, CLAP_EXT_PRESET_LOAD));
        x.host.output.clear();
        const std::string before = x.host.saveState();
        std::vector<std::pair<std::string, std::string>> refused; // file, key
        for (const auto &m : malformedCartridges(cart))
            refused.push_back({ "presets-" + m.first + ".syx", "0" });
        for (const char *key : { "32", "-1", "01", "", "x", "1 ", "100" })
            refused.push_back({ fixture, key });
        refused.push_back({ "presets-missing.syx", "0" });
        for (const auto &[file, key] : refused) {
            std::string why;
            check(preparePreset(x, "Bad", file, key.c_str(), &why) == CLAP_INVALID_ID && !why.empty(), "refused: " + file + " key '" + key + "'");
            check(!load->from_location(x.host.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file.c_str(), key.c_str()), "preset-load refused: " + file);
        }
        char message[256] = {};
        check(x.ctx->prepare_preset(x.host.plugin, "Bad", CLAP_PRESET_DISCOVERY_LOCATION_FILE, fixture.c_str(), nullptr, message, sizeof message) ==
                  CLAP_INVALID_ID, "no load key");
        check(x.ctx->prepare_preset(x.host.plugin, "Bad", CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "0", message, sizeof message) ==
                  CLAP_INVALID_ID, "no presets inside the plugin");
        x.run();
        check(x.host.saveState() == before, "refused preset loads leave the sound as it was");
        for (int k = 0; k < 7; k++)
            check(preparePreset(x, "Good", fixture, "0") != CLAP_INVALID_ID, "refused preparations took no context");
        std::string why;
        check(preparePreset(x, "One too many", fixture, "0", &why) == CLAP_INVALID_ID && why == "no free sound context", "contexts full");
    }

    std::printf("discovery: 2 locations, 32 voices named from the cartridge, 7 malformed files refused; prepare_preset and preset-load "
                "bit-identical for 32 voices; the snapshot opens without the cartridge; 16 bad presets refused\n");
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
            modulePath = args[i + 1];
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
    if (cmd == "rates")
        return crossRateTest();
    if (cmd == "render" || cmd == "render-stereo")
        return renderToWav(args, cmd == "render-stereo");
    if (cmd == "render-seq")
        return renderSeqToWav(args);
    if (cmd == "bench")
        return benchmark(args);
    if (cmd == "topology")
        return topologyTest();
    if (cmd == "contexts")
        return contextsTest();
    if (cmd == "state-active")
        return stateActiveTest();
    if (cmd == "snapshots")
        return snapshotsTest();
    if (cmd == "presets")
        return presetsTest();
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
