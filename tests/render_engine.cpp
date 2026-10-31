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
#include "msfa/exp2.h"

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

bool fail(const char *what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    return false;
}

struct NoEvents : SynthEngine::EventSource {
    void dispatchUntil(int) override {}
};

// Engine robustness: 8-operator graphs, prepare twice, corrupt algorithm numbers, sounds on
// blocks longer than prepared, portamento source.
int checkRobustness() {
    bool ok = true;
    SynthEngine tables; // builds the shared sine and exp tables the kernels below use
    // A synthetic 8-operator graph: 8 -> 7 -> 1 (carrier), 7 feeding back on itself while
    // modulated by 8.
    OperatorGraph g {};
    g.inputs[0] = 1u << 6;       // operator 1 <- 7
    g.inputs[6] = 1u << 7;       // operator 7 <- 8
    g.carriers = 1u << 0;
    g.feedbackFrom = g.feedbackTo = 7;
    const CompiledAlgorithm a = compileAlgorithm(g, 8);
    ok = (a.valid && a.feedbackOp == 8 - 7 && a.loopLength == 1) || fail("8-operator graph compiles");
    auto renderWith = [&](int shift) {
        FmCore core;
        FmOpParams params[8] {};
        for (auto &p : params) {
            p.level_in = 14 << 24 | (1 << 23); // well above the threshold
            p.freq = 1 << 22;
        }
        int32_t fb[2] = { 0, 0 };
        AlignedBuf<int32_t, N> bufs[8], sum;
        std::vector<int32_t> out;
        struct K {
            bool gains(FmOpParams &p, int32_t &g1, int32_t &g2) { g1 = p.gain_out; g2 = Exp2::lookup(p.level_in - (14 * (1 << 24))); p.gain_out = g2; return true; }
            void compute(int32_t *o, const int32_t *in, int32_t ph, int32_t f, int32_t g1, int32_t g2, bool add) { FmOpKernel::compute(o, in, ph, f, g1, g2, add); }
            void compute_pure(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, bool add) { FmOpKernel::compute_pure(o, ph, f, g1, g2, add); }
            void compute_fb(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *b, int sh, bool add) { FmOpKernel::compute_fb(o, ph, f, g1, g2, b, sh, add); }
            void compute_fb_mod(int32_t *o, const int32_t *in, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *b, int sh) { FmOpKernel::compute_fb_mod(o, in, ph, f, g1, g2, b, sh); }
            int selfFeedbackShift(int sh) { return sh; }
            bool loop(const CompiledAlgorithm &, FmOpParams *, int32_t, int32_t, int32_t *, int, AlignedBuf<int32_t, N> *, bool *, uint8_t &) { return false; }
        } k;
        for (int block = 0; block < 8; block++) {
            int32_t o[N] = {};
            renderGraph(k, a, o, params, fb, shift, bufs, sum.get());
            out.insert(out.end(), o, o + N);
        }
        return out;
    };
    {
        const auto with = renderWith(2), without = renderWith(16);
        long long energy = 0;
        for (int32_t x : without)
            energy += x < 0 ? -(long long)x : x;
        ok = (energy > 0 && with != without) || fail("a modulated operator keeps its feedback");
    }
    // a cycle in the inputs is refused
    OperatorGraph cyclic {};
    cyclic.inputs[0] = 1u << 1;  // 1 <- 2
    cyclic.inputs[1] = 1u << 0;  // 2 <- 1
    cyclic.carriers = 1u << 0;
    ok = !compileAlgorithm(cyclic, 8).valid || fail("a cyclic graph is refused");
    ok = (compileAlgorithm(dx7Algorithms[0], 6).valid) || fail("DX7 graphs are valid");

    // prepare twice (no release in between), a corrupt algorithm number, and a block longer
    // than prepared with a second sound playing
    uint8_t patch[156], patch2[156];
    const auto v = voice(3);
    std::memcpy(patch, v.data(), 155);
    patch[155] = 63;
    std::memcpy(patch2, patch, 156);
    patch2[134] = 200; // corrupt
    for (int op = 0; op < 6; op++)
        patch2[op * 21 + 7] = 0; // envelopes end at level 0, so the note can finish
    SynthEngine e;
    e.setTuning(createStandardTuning());
    e.setPatch(patch);
    e.setPatch(1, patch2);
    e.prepare(48000, 256);
    e.prepare(44100, 256); // ASan's leak check sees the first voices if they leak
    e.selectContext(1, 1);
    struct Sink : SynthEngine::NoteEndSink {
        int ended = 0;
        void noteEnded(const NoteRef &) override { ended++; }
    } sink;
    e.setNoteEndSink(&sink);
    NoteRef ref;
    ref.id = 7;
    ref.port = 0;
    ref.channel = 1;
    ref.key = 60;
    e.keydown(2, 60, 100, ref); // channel 2 plays context 1 (corrupt algorithm, clamped)
    std::vector<float> big(1000);
    NoEvents none;
    e.render(big.data(), 1000, none);
    bool heard = false;
    for (float x : big)
        heard = heard || x != 0;
    ok = heard || fail("a sound plays through a block longer than prepared");
    NoteRef all;
    e.releaseNotes(all);
    for (int k = 0; k < 400 && sink.ended == 0; k++) {
        e.render(big.data(), 1000, none);
        e.collectEndedNotes();
    }
    ok = (sink.ended == 1) || fail("its note ends (NOTE_END) through long blocks");
    e.release();
    std::printf("robustness: 8-operator feedback, cycles, prepare twice, corrupt algorithm, long blocks %s\n", ok ? "pass" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--robustness") == 0)
        return checkRobustness();
    if (argc == 3 && std::strcmp(argv[1], "--interleaved") == 0)
        return checkInterleaved(argv[2]);
    return runBaseline(argc, argv, render);
}
