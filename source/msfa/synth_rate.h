/*
 * Copyright 2026 the Oscairn contributors.
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

#ifndef SYNTH_RATE_H_
#define SYNTH_RATE_H_

#include "freqlut.h"
#include "porta.h"
#include "pitchenv.h"

// The sample-rate dependent tables and factors of one engine instance. They were
// process-wide statics, so two instances at different rates overwrote each other's.
struct SynthRate {
    Freqlut freqlut;
    Porta porta;
    uint32_t envMultiplier = 1 << 24; // Env::sr_multiplier
    int pitchEnvUnit = 0;             // PitchEnv::unit_
    double sampleRate = 44100;

    void init(double sampleRate) {
        this->sampleRate = sampleRate;
        freqlut.init(sampleRate);
        porta.init_sr(sampleRate);
        envMultiplier = (44100.0 / sampleRate) * (1<<24);
        pitchEnvUnit = PitchEnv::unitFor(sampleRate);
    }
};

#endif
