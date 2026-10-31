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

uint64_t render(const Case &c) {
    // What the plugin does: the patch is its 156-byte program (operator switch byte all on).
    uint8_t patch[156];
    const auto v = voice(c.voiceIndex);
    std::memcpy(patch, v.data(), 155);
    patch[155] = 63;

    SynthEngine engine;
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

    std::vector<float> left(c.block);
    BlockEvents events(engine);

    // Warm-up block: the plugin receives the voice (applyProgram: panic, LFO reset) and
    // re-reads it into the (silent) voices at the start of the next block.
    engine.panic();
    engine.resetLfo();
    engine.render(left.data(), c.block, events);
    engine.refreshVoices();

    const auto evs = dx7cases::events(c.scenario);
    const int total = (int)std::lround(scenarioSeconds(c.scenario) * c.rate);
    Hash hash;
    size_t next = 0;
    for (int pos = 0; pos < total; pos += c.block) {
        const int n = std::min(c.block, total - pos);
        events.pending.clear();
        events.next = 0;
        for (; next < evs.size() && eventSample(evs[next], c.rate) < pos + n; next++)
            events.pending.push_back({ eventSample(evs[next], c.rate) - pos, &evs[next] });
        engine.render(left.data(), n, events);
        // the plugin copies the mono output to both channels
        hash.add(left.data(), n);
        hash.add(left.data(), n);
    }
    engine.release();
    return hash.h;
}

} // namespace

int main(int argc, char **argv) {
    return runBaseline(argc, argv, render);
}
