/*
 * The Extended profile's part LFO (docs/oscairn-plan.md, milestone M6): one per sound, shared
 * by its voices like the DX7's LFO.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <cstdint>

struct ExtendedVoice;

class ExtPartLfo {
public:
    enum Wave { Triangle, TrianglePlus, SawUp, SawDown, Square14, Square13, Square, Square23, Square34,
                Trapezoid, SampleHold1, SampleHold2, User };

    void init(double sampleRate) { sampleRate_ = sampleRate; }
    // the sound's values (between blocks); tempo in beats per minute (tempo sync)
    void configure(const ExtendedVoice &v, double tempo);
    // a key-on of this sound; firstKey: no other key of the sound is held
    void keydown(bool firstKey);
    // n values (n <= 64) of the LFO times its envelope, -1..+1 (unipolar waves 0..+1 or -1..0)
    void render(float *out, int n);
    bool active() const { return active_; }

private:
    double wave(double phase); // one cycle, phase 0..1

    double sampleRate_ = 44100;
    double phase_ = 0;     // cycles, 0..1
    double inc_ = 0;       // cycles per sample
    double startPhase_ = 0;
    int wave_ = Triangle;
    int keyReset_ = 2;     // 0 off, 1 each key-on, 2 the first key-on
    // the envelope: delay, fade in, hold (unless held for ever), fade out; samples per stage
    double stage_[4] = {};
    bool holdForever_ = true;
    int envStage_ = 4;     // 0 delay, 1 fade in, 2 hold, 3 fade out, 4 done (level 1 or 0)
    double envPos_ = 0;    // samples into the stage
    double envDone_ = 1;   // the level once done
    // sample and hold: the held value and the cycle it belongs to
    double held_ = 0;
    uint32_t random_ = 0x6D2B79F5u;
    // the user wave
    int steps_ = 16, slope_ = 0;
    float stepValue_[16] = {};
    bool active_ = false;  // a destination has depth
};
