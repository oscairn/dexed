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

#include <atomic>
#include <cmath>
#include <fstream>
#include <new>
#include <set>
#include <sstream>
#include <thread>
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

    // Choke ends a held note at once, at the event's frame.
    h.output.clear();
    in.note(CLAP_EVENT_NOTE_CHOKE, 100, 8, 0, 60);
    h.process(256, in);
    in.clear();
    e = ends(h);
    check(e.size() == 1 && e[0].id == 8 && e[0].time == 100, "choke gives NOTE_END for 8 at frame 100");

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
    std::printf("main-thread parameter reads, saves and loads while processing\n");
    return 0;
}

// ---- algorithm topology ----

// Renders note 60 with only the operators in `on` at full level (simple ratio-1 sine
// operators with instant envelopes), with the given algorithm, feedback and engine.
std::vector<float> renderOperators(int algorithm, unsigned on, int feedback, int engine) {
    Host h(entry);
    h.activate(48000, 512);
    EventList in;
    in.events.reserve(256);
    in.param(0, 1, algorithm);
    in.param(0, 2, feedback);
    in.param(0, 100, engine);
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

int topologyTest() {
    int failures = 0;
    auto expect = [&](bool ok, int alg, int engine, const std::string &what) {
        if (!ok) {
            std::fprintf(stderr, "algorithm %d, engine %d: %s\n", alg, engine, what.c_str());
            failures++;
        }
    };
    for (int a = 1; a <= 32; a++) {
        const OperatorGraph &g = dx7Algorithms[a - 1];
        // ancestors[k]: every operator whose output reaches operator k
        unsigned ancestors[6];
        for (int k = 0; k < 6; k++) {
            unsigned m = g.inputs[k], prev = 0;
            while (m != prev) {
                prev = m;
                for (int j = 0; j < 6; j++)
                    if (m & (1u << j))
                        m |= g.inputs[j];
            }
            ancestors[k] = m;
        }
        for (int engine = 0; engine < 3; engine++) {
            std::vector<float> alone[6];
            for (int k = 0; k < 6; k++) {
                alone[k] = renderOperators(a, 1u << k, 0, engine);
                const bool carrier = (g.carriers >> k) & 1;
                expect(audible(alone[k]) == carrier, a, engine, "operator " + std::to_string(k + 1) + (carrier ? " should" : " should not") + " be heard alone");
            }
            // Every edge: with operator m and everything it reaches on (its path to the
            // output), adding operator j changes that path iff j directly modulates an
            // operator on it; otherwise j is just heard beside it (or not at all).
            for (int m = 0; m < 6; m++) {
                unsigned path = 1u << m, modulators = 0;
                for (int k = 0; k < 6; k++)
                    if ((ancestors[k] >> m) & 1)
                        path |= 1u << k;
                for (int k = 0; k < 6; k++)
                    if ((path >> k) & 1)
                        modulators |= g.inputs[k];
                const auto base = renderOperators(a, path, 0, engine);
                for (int j = 0; j < 6; j++) {
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
            for (int k = 0; k < 6; k++)
                if ((ancestors[k] >> to) & 1)
                    path |= 1u << k;
            path |= 1u << (g.feedbackFrom - 1);
            expect(renderOperators(a, path, 7, engine) != renderOperators(a, path, 0, engine), a, engine, "feedback changes nothing");
        }
    }
    std::printf("32 algorithms x 3 engines: carriers, modulation paths and feedback %s\n", failures ? "DIFFER" : "match the graphs");
    return failures == 0 ? 0 : 1;
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
    if (cmd == "topology")
        return topologyTest();
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
