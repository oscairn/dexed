/*
 * Copyright 2013 Google Inc.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Low frequency oscillator, compatible with DX7

#pragma once

#include <cstdint>

struct ExtendedVoice;

class Lfo {
public:
    void init(double sample_rate);

    void reset(const uint8_t params[6]);
    // The Extended profile's LFO: measured speed tables, start phase, DX7 delay and waves.
    void reset(const ExtendedVoice &v);
    // The rate of DX7 LFO speed 0-99 as this class plays it, in Hz.
    static double classicHz(int speed);
    // After reset(ExtendedVoice) the LFO advances once per sample (getsample() N times a
    // block), so extended speeds up to 1357 Hz play; otherwise once per block.
    bool perSample() const { return perSample_; }

    // result is 0..1 in Q24
    int32_t getsample();

    // result is 0..1 in Q24
    int32_t getdelay();

    void keydown();
    // Extended voices each run their own copy of the sound's LFO (measured: the 2nd LFO is per
    // voice); at its note-on a copy takes the sound's free-running phase, then keydown()
    uint32_t phase() const { return phase_; }
    void start(uint32_t phase) {
        phase_ = phase;
        keydown();
    }
    // Extended sounds: the 2nd LFO's speed moved by this many steps (the part LFO's LFO 2 speed
    // destination); speed range Normal only
    void setSpeedSteps(int steps);

private:
    // per instance: they depend on the sample rate (they used to be process-wide)
    uint32_t lforatio_ = 0;
    uint32_t unit_ = 0;
    double hzToDelta_ = 0; // phase increment per sample for 1 Hz
    bool perSample_ = false;
    uint32_t startPhase_ = (1U << 31) - 1; // the phase a key-on reset starts from
    int extSpeed_ = -1;                    // Extended, speed range Normal: the speed (else -1)
    int speedSteps_ = 0;

    // phase, S/H and delay state used to be read uninitialised until a synced key-down
    uint32_t phase_ = 0; // Q32
    uint32_t delta_;
    uint8_t waveform_;
    uint8_t randstate_ = 0;
    bool sync_;

    uint32_t delaystate_ = 0;
    uint32_t delayinc_;
    uint32_t delayinc2_;
};
