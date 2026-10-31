/*
 * dexed-render-plugin: renders the generated DX7 cases through the JUCE plugin
 * (createPluginFilter, black box: voices arrive as single-voice SysEx, settings as
 * parameters) and writes or checks one hash per case.
 *
 *   dexed-render-plugin --write baseline.txt
 *   dexed-render-plugin --check baseline.txt [--reverse]
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "dx7_cases.h"
#include "baseline_file.h"

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter();

using namespace dx7cases;

static void setParam(juce::AudioProcessor &p, const juce::String &id, float displayValue) {
    for (auto *param : p.getParameters()) {
        auto *ranged = dynamic_cast<juce::RangedAudioParameter *>(param);
        if (ranged != nullptr && ranged->paramID == id) {
            ranged->setValueNotifyingHost(ranged->convertTo0to1(displayValue));
            return;
        }
    }
    std::fprintf(stderr, "no parameter %s\n", id.toRawUTF8());
    std::exit(2);
}

static uint64_t render(const Case &c) {
    std::unique_ptr<juce::AudioProcessor> proc(createPluginFilter());
    proc->setPlayConfigDetails(0, 2, c.rate, c.block);
    proc->prepareToPlay(c.rate, c.block);

    const Settings &s = c.settings;
    setParam(*proc, "engineType", (float)s.engine);
    setParam(*proc, "monoMode", s.mono ? 1.0f : 0.0f);
    setParam(*proc, "portamentoTm", (float)s.portamentoTime);
    setParam(*proc, "modWheel", (float)s.wheelRange);
    setParam(*proc, "modWheelPitch", s.wheelPitch ? 1.0f : 0.0f);
    setParam(*proc, "modWheelAmp", s.wheelAmp ? 1.0f : 0.0f);
    setParam(*proc, "modAftertouch", (float)s.atRange);
    setParam(*proc, "modAftertouchEgBias", s.atEgBias ? 1.0f : 0.0f);
    setParam(*proc, "pitchRangeUp", (float)s.pitchUp);
    setParam(*proc, "pitchRangeDown", (float)s.pitchDown);

    juce::AudioBuffer<float> buffer(2, c.block);
    juce::MidiBuffer midi;

    // Warm-up block: the voice arrives as SysEx, nothing plays.
    auto sysex = singleVoiceSysex(voice(c.voiceIndex));
    midi.addEvent(juce::MidiMessage::createSysExMessage(sysex.data() + 1, (int)sysex.size() - 2), 0);
    buffer.clear();
    proc->processBlock(buffer, midi);

    const auto evs = events(c.scenario);
    const int total = (int)std::lround(scenarioSeconds(c.scenario) * c.rate);
    Hash hash;
    size_t next = 0;
    for (int pos = 0; pos < total; pos += c.block) {
        const int n = std::min(c.block, total - pos);
        buffer.setSize(2, n, false, false, true);
        buffer.clear();
        midi.clear();
        for (; next < evs.size() && eventSample(evs[next], c.rate) < pos + n; next++)
            midi.addEvent(evs[next].bytes, evs[next].size, eventSample(evs[next], c.rate) - pos);
        proc->processBlock(buffer, midi);
        hash.add(buffer.getReadPointer(0), n);
        hash.add(buffer.getReadPointer(1), n);
    }
    proc->releaseResources();
    return hash.h;
}

int main(int argc, char **argv) {
    juce::ScopedJuceInitialiser_GUI juce;
    return runBaseline(argc, argv, render);
}
