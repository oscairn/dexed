/*
 * dexed-render-engine: renders the generated DX7 cases with the headless engine library
 * alone (no JUCE) and writes or checks one hash per case, against the same baseline as
 * dexed-render-plugin.
 *
 *   dexed-render-engine --write baseline.txt
 *   dexed-render-engine --check baseline.txt [--reverse]
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "engine/SynthEngine.h"

#include "dx7_cases.h"
#include "baseline_file.h"

#include <cstring>

using namespace dx7cases;

namespace {

// Short channel messages at sample offsets within one block.
struct BlockEvents : SynthEngine::EventSource {
    SynthEngine &engine;
    std::vector<std::pair<int, const Event *>> pending;
    size_t next = 0;
    explicit BlockEvents(SynthEngine &e) : engine(e) {}
    void dispatchUntil(int samplePos) override {
        for (; next < pending.size() && pending[next].first <= samplePos; next++) {
            const Event &e = *pending[next].second;
            engine.processMidiMessage(e.bytes, (e.bytes[0] & 0x0f) + 1);
        }
    }
};

// Renders one case block by block, as the plugin would.
struct CaseRenderer {
    const Case &c;
    uint8_t patch[156];
    SynthEngine engine;
    BlockEvents events { engine };
    std::vector<float> left;
    std::vector<Event> evs;
    int total = 0, pos = 0;
    size_t next = 0;
    Hash hash;

    explicit CaseRenderer(const Case &c) : c(c), left(c.block), evs(dx7cases::events(c.scenario)) {
        // What the plugin does: the patch is its 156-byte program (operator switch byte all on).
        const auto v = voice(c.voiceIndex);
        std::memcpy(patch, v.data(), 155);
        patch[155] = 63;

        engine.setTuning(createStandardTuning());
        engine.setPatch(patch);
        engine.prepare(c.rate);

        // The plugin's parameter listeners (source/parameter/ParametersMapper.cpp).
        const Settings &s = c.settings;
        Controllers &ctrl = engine.controllers;
        engine.setEngineType(s.engine);
        engine.setMonoMode(s.mono);
        ctrl.portamento_cc = static_cast<int32_t>(s.portamentoTime * 127.0f / 99.0f);
        ctrl.portamento_enable_cc = ctrl.portamento_cc > 0;
        ctrl.wheel.range = s.wheelRange;
        ctrl.wheel.pitch = s.wheelPitch;
        ctrl.wheel.amp = s.wheelAmp;
        ctrl.at.range = s.atRange;
        ctrl.at.eg = s.atEgBias;
        ctrl.refresh();
        ctrl.pitch_range_up = s.pitchUp;
        ctrl.pitch_range_dn = s.pitchDown;

        // Warm-up block: the plugin receives the voice (applyProgram: panic, LFO reset) and
        // re-reads it into the (silent) voices at the start of the next block.
        engine.panic();
        engine.resetLfo();
        engine.render(left.data(), c.block, events);
        engine.refreshVoices();

        total = (int)std::lround(scenarioSeconds(c.scenario) * c.rate);
    }

    // Renders the next block; false when the case is finished.
    bool step() {
        if (pos >= total)
            return false;
        const int n = std::min(c.block, total - pos);
        events.pending.clear();
        events.next = 0;
        for (; next < evs.size() && eventSample(evs[next], c.rate) < pos + n; next++)
            events.pending.push_back({ eventSample(evs[next], c.rate) - pos, &evs[next] });
        engine.render(left.data(), n, events);
        // the plugin copies the mono output to both channels
        hash.add(left.data(), n);
        hash.add(left.data(), n);
        pos += n;
        return true;
    }
};

uint64_t render(const Case &c) {
    CaseRenderer r(c);
    while (r.step()) {}
    return r.hash.h;
}

// Two instances at different sample rates, rendering block by block in turn: each must
// still match its own baseline (the rate tables used to be shared by the whole process).
int checkInterleaved(const char *baseline) {
    std::map<std::string, uint64_t> expected;
    std::ifstream in(baseline);
    std::string name, hex;
    while (in >> name >> hex)
        if (name[0] != '#')
            expected[name] = std::strtoull(hex.c_str(), nullptr, 16);
        else
            std::getline(in, hex);
    const auto all = cases();
    int failures = 0, pairs = 0;
    for (const auto &a : all) {
        if (a.rate == 48000)
            continue;
        const Case &b = all[pairs % 120]; // a 48 kHz case
        CaseRenderer ra(a), rb(b);
        bool more = true;
        while (more) {
            more = ra.step();
            more = rb.step() || more;
        }
        for (const auto *r : { &ra, &rb })
            if (expected[r->c.name] != r->hash.h) {
                std::fprintf(stderr, "DIFF %s (interleaved)\n", r->c.name.c_str());
                failures++;
            }
        pairs++;
    }
    std::printf("%d interleaved pairs, %d differ\n", pairs, failures);
    return failures == 0 && pairs > 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--interleaved") == 0)
        return checkInterleaved(argv[2]);
    return runBaseline(argc, argv, render);
}
