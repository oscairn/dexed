/*
 * Generated DX7 voices and playing scenarios shared by the render tests.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * SPDX-License-Identifier: MIT
 * Test helper, MIT licensed so any project can reuse it (docs/oscairn-plan.md, section 10).
 *
 * The voices are generated from a fixed seed, never taken from a ROM or a
 * factory bank. The generator and the scenarios must never change once a
 * baseline has been recorded from them: changing them invalidates
 * the files in tests/baseline/.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dx7cases {

// Small deterministic generator (PCG32), so voices are the same on every platform
// (std:: distributions are implementation-defined).
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = (uint32_t)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
    }
    int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
};

// One unpacked 155-byte DX7 voice. Operators are stored op 6 first, as in DX7 SysEx.
// Every voice uses LFO key sync and waves 0-4: in unmodified Dexed the shared LFO's phase
// and S/H state were uninitialised until a synced key-down, so free-running and S/H LFOs did
// not render reproducibly. That state is initialised now; the cases keep these limits so the
// baseline matches what unmodified Dexed rendered.
inline std::vector<uint8_t> voice(int index) {
    Rng rng(0xDE7EDULL + (uint64_t)index * 7919ULL);
    std::vector<uint8_t> v(155, 0);
    for (int op = 0; op < 6; op++) {
        uint8_t *o = v.data() + op * 21;
        o[0] = (uint8_t)rng.range(45, 99);  // R1
        o[1] = (uint8_t)rng.range(20, 99);  // R2
        o[2] = (uint8_t)rng.range(10, 99);  // R3
        o[3] = (uint8_t)rng.range(35, 99);  // R4
        o[4] = (uint8_t)rng.range(60, 99);  // L1
        o[5] = (uint8_t)rng.range(0, 99);   // L2
        o[6] = (uint8_t)rng.range(0, 99);   // L3
        o[7] = (uint8_t)(rng.range(0, 3) == 0 ? rng.range(0, 40) : 0); // L4
        o[8] = (uint8_t)rng.range(0, 99);   // break point
        o[9] = (uint8_t)rng.range(0, 99);   // left depth
        o[10] = (uint8_t)rng.range(0, 99);  // right depth
        o[11] = (uint8_t)rng.range(0, 3);   // left curve
        o[12] = (uint8_t)rng.range(0, 3);   // right curve
        o[13] = (uint8_t)rng.range(0, 7);   // rate scaling
        o[14] = (uint8_t)rng.range(0, 3);   // amp mod sens
        o[15] = (uint8_t)rng.range(0, 7);   // key velocity sens
        o[16] = (uint8_t)rng.range(50, 99); // output level
        o[17] = (uint8_t)(rng.range(0, 7) == 0 ? 1 : 0); // fixed mode, sometimes
        o[18] = (uint8_t)rng.range(0, 31);  // coarse
        o[19] = (uint8_t)rng.range(0, 99);  // fine
        o[20] = (uint8_t)rng.range(0, 14);  // detune
    }
    for (int i = 0; i < 4; i++) v[126 + i] = (uint8_t)rng.range(30, 99); // pitch EG rates
    for (int i = 0; i < 4; i++) v[130 + i] = (uint8_t)(rng.range(0, 2) == 0 ? rng.range(30, 70) : 50); // levels
    v[133] = 50;                              // pitch EG starts and ends at centre
    v[134] = (uint8_t)(index % 32);           // algorithm: every one is covered
    v[135] = (uint8_t)rng.range(0, 7);        // feedback
    v[136] = (uint8_t)rng.range(0, 1);        // osc key sync
    v[137] = (uint8_t)rng.range(0, 99);       // LFO speed
    v[138] = (uint8_t)rng.range(0, 99);       // LFO delay
    v[139] = (uint8_t)rng.range(0, 99);       // LFO PMD
    v[140] = (uint8_t)rng.range(0, 99);       // LFO AMD
    v[141] = 1;                               // LFO key sync (see above)
    v[142] = (uint8_t)rng.range(0, 4);        // LFO wave (no S/H, see above)
    v[143] = (uint8_t)rng.range(0, 7);        // pitch mod sens
    v[144] = (uint8_t)rng.range(18, 30);      // transpose around C3
    char name[11];
    std::snprintf(name, sizeof name, "GEN %06d", index);
    for (int i = 0; i < 10; i++) v[145 + i] = (uint8_t)name[i];
    return v;
}

// A single-voice SysEx dump of the voice (F0 43 00 00 01 1B ... checksum F7).
inline std::vector<uint8_t> singleVoiceSysex(const std::vector<uint8_t> &v) {
    std::vector<uint8_t> m = { 0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B };
    int sum = 0;
    for (uint8_t b : v) { m.push_back(b); sum += b; }
    m.push_back((uint8_t)((128 - (sum & 127)) & 127));
    m.push_back(0xF7);
    return m;
}

struct Event {
    double seconds;
    uint8_t bytes[3];
    int size;
};

enum class Scenario { Poly, Mono, Mpe };

inline const char *scenarioName(Scenario s) {
    return s == Scenario::Poly ? "poly" : s == Scenario::Mono ? "mono" : "mpe";
}

inline double scenarioSeconds(Scenario s) { return s == Scenario::Poly ? 1.6 : 1.5; }

inline std::vector<Event> events(Scenario s) {
    auto on = [](double t, int ch, int key, int vel) { return Event{ t, { (uint8_t)(0x90 | (ch - 1)), (uint8_t)key, (uint8_t)vel }, 3 }; };
    auto off = [](double t, int ch, int key) { return Event{ t, { (uint8_t)(0x80 | (ch - 1)), (uint8_t)key, 64 }, 3 }; };
    auto cc = [](double t, int ch, int c, int val) { return Event{ t, { (uint8_t)(0xB0 | (ch - 1)), (uint8_t)c, (uint8_t)val }, 3 }; };
    auto bend = [](double t, int ch, int val) { return Event{ t, { (uint8_t)(0xE0 | (ch - 1)), (uint8_t)(val & 127), (uint8_t)(val >> 7) }, 3 }; };
    auto at = [](double t, int ch, int val) { return Event{ t, { (uint8_t)(0xD0 | (ch - 1)), (uint8_t)val, 0 }, 2 }; };
    std::vector<Event> e;
    switch (s) {
    case Scenario::Poly:
        e = { on(0.0, 1, 60, 100), on(0.05, 1, 64, 64), on(0.10, 1, 67, 127),
              cc(0.20, 1, 1, 90), bend(0.30, 1, 0x3000), at(0.35, 1, 70),
              off(0.45, 1, 60), off(0.45, 1, 64), off(0.45, 1, 67),
              cc(0.50, 1, 64, 127), on(0.52, 1, 72, 90), off(0.60, 1, 72), cc(0.70, 1, 64, 0) };
        // 18 overlapping notes on 16 voices: voice stealing
        for (int i = 0; i < 18; i++) e.push_back(on(0.75 + 0.01 * i, 1, 36 + i, 30 + 5 * i));
        e.push_back(bend(1.05, 1, 0x2000));
        e.push_back(cc(1.05, 1, 1, 0));
        e.push_back(cc(1.10, 1, 123, 0));
        break;
    case Scenario::Mono:
        e = { cc(0.0, 1, 65, 127), on(0.0, 1, 60, 100), on(0.15, 1, 67, 90), on(0.30, 1, 55, 80),
              off(0.45, 1, 67), off(0.60, 1, 55), off(0.75, 1, 60), on(0.90, 1, 72, 110), off(1.20, 1, 72) };
        break;
    case Scenario::Mpe:
        e = { on(0.0, 2, 60, 100), on(0.05, 3, 64, 90), on(0.10, 4, 67, 80), bend(0.30, 3, 0x3000),
              bend(0.40, 4, 0x1000), off(0.60, 2, 60), off(0.70, 3, 64), off(0.80, 4, 67) };
        break;
    }
    return e;
}

// The plugin-level settings a case uses. Both harnesses apply exactly these.
struct Settings {
    int engine;         // 0 Modern, 1 Mark I, 2 OPL
    bool mono;
    int portamentoTime; // 0-99 (Dexed's PORTA TIME parameter)
    int wheelRange = 80; bool wheelPitch = true; bool wheelAmp = true;
    int atRange = 60; bool atEgBias = true;
    int pitchUp = 4, pitchDown = 7;
};

struct Case {
    std::string name;
    int voiceIndex;
    Scenario scenario;
    Settings settings;
    double rate;
    int block;
};

inline std::vector<Case> cases() {
    std::vector<Case> c;
    auto add = [&](int v, Scenario s, int engine, double rate, int block) {
        Settings st{ engine, s == Scenario::Mono, s == Scenario::Mono ? 40 : 0 };
        char name[96];
        std::snprintf(name, sizeof name, "v%02d-%s-e%d-%d-b%d", v, scenarioName(s), engine, (int)rate, block);
        c.push_back({ name, v, s, st, rate, block });
    };
    for (int v = 0; v < 40; v++)
        for (int e = 0; e < 3; e++)
            add(v, Scenario::Poly, e, 48000, 512);
    for (int v = 0; v < 8; v++) {
        add(v * 5, Scenario::Poly, 1, 44100, 37);
        add(v * 5 + 1, Scenario::Poly, 0, 96000, 1024);
        for (int e = 0; e < 3; e++)
            add(v * 3 + 2, Scenario::Mono, e, 48000, 256);
    }
    for (int v = 0; v < 4; v++)
        add(v * 7 + 3, Scenario::Mpe, 1, 48000, 100);
    return c;
}

// FNV-1a 64 over the raw bytes of the rendered floats.
struct Hash {
    uint64_t h = 1469598103934665603ULL;
    void add(const float *x, int n) {
        const auto *b = reinterpret_cast<const unsigned char *>(x);
        for (size_t i = 0; i < (size_t)n * sizeof(float); i++) { h ^= b[i]; h *= 1099511628211ULL; }
    }
};

inline int eventSample(const Event &e, double rate) { return (int)std::lround(e.seconds * rate); }

} // namespace dx7cases
