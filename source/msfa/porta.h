/*
   Copyright 2019 Jean Pierre Cimalando.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef SYNTH_PORTA_H_
#define SYNTH_PORTA_H_

#include <stdint.h>

// One set per engine instance: the rates depend on the sample rate (they used to be process-wide).
struct Porta {
  public:
    void init_sr(double sampleRate);
    int32_t rates[128];
    int32_t rates_glissando[128];
};

#endif
