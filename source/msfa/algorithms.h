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

#ifndef SYNTH_ALGORITHMS_H_
#define SYNTH_ALGORITHMS_H_

#include <stdint.h>

// An algorithm as an operator graph (docs/oscairn-plan.md, section 3.6). Operators are
// numbered from 1 as on the panel; bit k of a mask is operator k + 1.
struct OperatorGraph {
    uint8_t inputs[8];     // the operators that modulate each operator
    uint8_t carriers;      // the operators heard at the output
    uint8_t feedbackFrom;  // feedback goes from this operator...
    uint8_t feedbackTo;    // ...back into this one (the same operator: self feedback)
};

#define OP(n) (1u << ((n) - 1))

// The 32 DX7 algorithms, the same graphs as the bus flags in FmCore::algorithms (derived from
// them and checked against renders by tests/oscairn_fm_test.cpp "topology").
static const OperatorGraph dx7Algorithms[32] = {
    /*  1 */ { { OP(2), 0, OP(4), OP(5), OP(6), 0 }, OP(1) | OP(3), 6, 6 },
    /*  2 */ { { OP(2), 0, OP(4), OP(5), OP(6), 0 }, OP(1) | OP(3), 2, 2 },
    /*  3 */ { { OP(2), OP(3), 0, OP(5), OP(6), 0 }, OP(1) | OP(4), 6, 6 },
    /*  4 */ { { OP(2), OP(3), 0, OP(5), OP(6), 0 }, OP(1) | OP(4), 4, 6 },
    /*  5 */ { { OP(2), 0, OP(4), 0, OP(6), 0 }, OP(1) | OP(3) | OP(5), 6, 6 },
    /*  6 */ { { OP(2), 0, OP(4), 0, OP(6), 0 }, OP(1) | OP(3) | OP(5), 5, 6 },
    /*  7 */ { { OP(2), 0, OP(4) | OP(5), 0, OP(6), 0 }, OP(1) | OP(3), 6, 6 },
    /*  8 */ { { OP(2), 0, OP(4) | OP(5), 0, OP(6), 0 }, OP(1) | OP(3), 4, 4 },
    /*  9 */ { { OP(2), 0, OP(4) | OP(5), 0, OP(6), 0 }, OP(1) | OP(3), 2, 2 },
    /* 10 */ { { OP(2), OP(3), 0, OP(5) | OP(6), 0, 0 }, OP(1) | OP(4), 3, 3 },
    /* 11 */ { { OP(2), OP(3), 0, OP(5) | OP(6), 0, 0 }, OP(1) | OP(4), 6, 6 },
    /* 12 */ { { OP(2), 0, OP(4) | OP(5) | OP(6), 0, 0, 0 }, OP(1) | OP(3), 2, 2 },
    /* 13 */ { { OP(2), 0, OP(4) | OP(5) | OP(6), 0, 0, 0 }, OP(1) | OP(3), 6, 6 },
    /* 14 */ { { OP(2), 0, OP(4), OP(5) | OP(6), 0, 0 }, OP(1) | OP(3), 6, 6 },
    /* 15 */ { { OP(2), 0, OP(4), OP(5) | OP(6), 0, 0 }, OP(1) | OP(3), 2, 2 },
    /* 16 */ { { OP(2) | OP(3) | OP(5), 0, OP(4), 0, OP(6), 0 }, OP(1), 6, 6 },
    /* 17 */ { { OP(2) | OP(3) | OP(5), 0, OP(4), 0, OP(6), 0 }, OP(1), 2, 2 },
    /* 18 */ { { OP(2) | OP(3) | OP(4), 0, 0, OP(5), OP(6), 0 }, OP(1), 3, 3 },
    /* 19 */ { { OP(2), OP(3), 0, OP(6), OP(6), 0 }, OP(1) | OP(4) | OP(5), 6, 6 },
    /* 20 */ { { OP(3), OP(3), 0, OP(5) | OP(6), 0, 0 }, OP(1) | OP(2) | OP(4), 3, 3 },
    /* 21 */ { { OP(3), OP(3), 0, OP(6), OP(6), 0 }, OP(1) | OP(2) | OP(4) | OP(5), 3, 3 },
    /* 22 */ { { OP(2), 0, OP(6), OP(6), OP(6), 0 }, OP(1) | OP(3) | OP(4) | OP(5), 6, 6 },
    /* 23 */ { { 0, OP(3), 0, OP(6), OP(6), 0 }, OP(1) | OP(2) | OP(4) | OP(5), 6, 6 },
    /* 24 */ { { 0, 0, OP(6), OP(6), OP(6), 0 }, OP(1) | OP(2) | OP(3) | OP(4) | OP(5), 6, 6 },
    /* 25 */ { { 0, 0, 0, OP(6), OP(6), 0 }, OP(1) | OP(2) | OP(3) | OP(4) | OP(5), 6, 6 },
    /* 26 */ { { 0, OP(3), 0, OP(5) | OP(6), 0, 0 }, OP(1) | OP(2) | OP(4), 6, 6 },
    /* 27 */ { { 0, OP(3), 0, OP(5) | OP(6), 0, 0 }, OP(1) | OP(2) | OP(4), 3, 3 },
    /* 28 */ { { OP(2), 0, OP(4), OP(5), 0, 0 }, OP(1) | OP(3) | OP(6), 5, 5 },
    /* 29 */ { { 0, 0, OP(4), 0, OP(6), 0 }, OP(1) | OP(2) | OP(3) | OP(5), 6, 6 },
    /* 30 */ { { 0, 0, OP(4), OP(5), 0, 0 }, OP(1) | OP(2) | OP(3) | OP(6), 5, 5 },
    /* 31 */ { { 0, 0, 0, 0, OP(6), 0 }, OP(1) | OP(2) | OP(3) | OP(4) | OP(5), 6, 6 },
    /* 32 */ { { 0, 0, 0, 0, 0, 0 }, OP(1) | OP(2) | OP(3) | OP(4) | OP(5) | OP(6), 6, 6 },
};

// Operators that FmCore::isCarrier also counted as carriers (those that add into bus 1 or 2),
// so Dx7Note::isPlaying(), and with it voice stealing, stays as it was for classic voices.
static const uint8_t dx7ExtraActive[32] = {
    0, 0, 0, 0, 0, 0, OP(4), OP(4), OP(4), OP(5), OP(5), OP(5) | OP(4), OP(5) | OP(4), OP(5), OP(5),
    OP(3) | OP(2), OP(3) | OP(2), OP(3) | OP(2), 0, OP(5), 0, 0, 0, 0, 0, OP(5), OP(5), 0, 0, 0, 0, 0,
};

// The 88 Extended algorithms, 8 operators each. Operators modulate only higher-numbered
// operators, so ascending order renders them; feedback runs from feedbackFrom back into
// feedbackTo (two algorithms, 12 and 14, have a loop through other operators). Transcribed
// from the published FM-X algorithm chart; tests/oscairn_fm_test.cpp "topology" checks the
// renders against these graphs, and docs/oscairn-plan.md section 11 records the measurement
// against reference renders.
static const OperatorGraph extAlgorithms[88] = {
    /*  1 */ { { 0, 0, 0, 0, 0, 0, 0, 0 }, OP(1) | OP(2) | OP(3) | OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /*  2 */ { { 0, OP(1), OP(2), OP(3), 0, 0, 0, 0 }, OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /*  3 */ { { 0, 0, OP(1) | OP(2), OP(3), 0, 0, 0, 0 }, OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /*  4 */ { { 0, 0, OP(2), OP(1) | OP(3), 0, 0, 0, 0 }, OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /*  5 */ { { 0, OP(1), 0, OP(2) | OP(3), 0, 0, 0, 0 }, OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /*  6 */ { { 0, OP(1), 0, OP(3), 0, 0, 0, 0 }, OP(2) | OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /*  7 */ { { 0, OP(1), OP(1), OP(1), 0, 0, 0, 0 }, OP(2) | OP(3) | OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /*  8 */ { { 0, OP(1), 0, 0, 0, 0, 0, 0 }, OP(2) | OP(3) | OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /*  9 */ { { 0, 0, 0, OP(3), OP(4), OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(6) | OP(8), 3, 3 },
    /* 10 */ { { 0, 0, 0, OP(3), OP(4), OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(6) | OP(8), 7, 7 },
    /* 11 */ { { 0, 0, 0, OP(3), OP(4), 0, OP(6), OP(7) }, OP(1) | OP(2) | OP(5) | OP(8), 3, 3 },
    /* 12 */ { { 0, 0, 0, OP(3), OP(4), 0, OP(6), OP(7) }, OP(1) | OP(2) | OP(5) | OP(8), 5, 3 },
    /* 13 */ { { 0, 0, 0, OP(3), 0, OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(4) | OP(6) | OP(8), 3, 3 },
    /* 14 */ { { 0, 0, 0, OP(3), 0, OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(4) | OP(6) | OP(8), 4, 3 },
    /* 15 */ { { 0, 0, 0, OP(3), 0, OP(4) | OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(6) | OP(8), 3, 3 },
    /* 16 */ { { 0, 0, 0, OP(3), 0, OP(4) | OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(6) | OP(8), 5, 5 },
    /* 17 */ { { 0, 0, 0, OP(3), 0, OP(4) | OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(6) | OP(8), 7, 7 },
    /* 18 */ { { 0, 0, 0, OP(3), OP(4), 0, 0, OP(6) | OP(7) }, OP(1) | OP(2) | OP(5) | OP(8), 3, 3 },
    /* 19 */ { { 0, 0, 0, OP(3), OP(4), 0, 0, OP(6) | OP(7) }, OP(1) | OP(2) | OP(5) | OP(8), 7, 7 },
    /* 20 */ { { 0, 0, 0, OP(3), 0, 0, 0, OP(5) | OP(6) | OP(7) }, OP(1) | OP(2) | OP(4) | OP(8), 3, 3 },
    /* 21 */ { { 0, 0, 0, OP(3), 0, 0, 0, OP(5) | OP(6) | OP(7) }, OP(1) | OP(2) | OP(4) | OP(8), 5, 5 },
    /* 22 */ { { 0, 0, 0, 0, OP(3) | OP(4), OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(6) | OP(8), 3, 3 },
    /* 23 */ { { 0, 0, 0, 0, OP(3) | OP(4), OP(5), 0, OP(7) }, OP(1) | OP(2) | OP(6) | OP(8), 7, 7 },
    /* 24 */ { { 0, 0, 0, OP(3), 0, OP(5), 0, OP(4) | OP(6) | OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 25 */ { { 0, 0, 0, OP(3), 0, OP(5), 0, OP(4) | OP(6) | OP(7) }, OP(1) | OP(2) | OP(8), 7, 7 },
    /* 26 */ { { 0, 0, 0, OP(3), OP(4), 0, 0, OP(5) | OP(6) | OP(7) }, OP(1) | OP(2) | OP(8), 6, 6 },
    /* 27 */ { { 0, 0, 0, OP(3), OP(3), 0, OP(6), OP(7) }, OP(1) | OP(2) | OP(4) | OP(5) | OP(8), 3, 3 },
    /* 28 */ { { 0, 0, 0, 0, OP(3) | OP(4), 0, OP(6), 0 }, OP(1) | OP(2) | OP(5) | OP(7) | OP(8), 6, 6 },
    /* 29 */ { { 0, 0, 0, OP(3), OP(3), 0, OP(6), OP(6) }, OP(1) | OP(2) | OP(4) | OP(5) | OP(7) | OP(8), 6, 6 },
    /* 30 */ { { 0, 0, 0, OP(3), OP(3), OP(3), 0, OP(7) }, OP(1) | OP(2) | OP(4) | OP(5) | OP(6) | OP(8), 3, 3 },
    /* 31 */ { { 0, OP(1), OP(1), 0, OP(4), 0, 0, 0 }, OP(2) | OP(3) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 32 */ { { 0, OP(1), OP(1), 0, 0, 0, 0, 0 }, OP(2) | OP(3) | OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 33 */ { { 0, 0, OP(1) | OP(2), 0, OP(4), 0, 0, 0 }, OP(3) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 34 */ { { 0, 0, OP(1) | OP(2), 0, OP(4), 0, 0, 0 }, OP(3) | OP(5) | OP(6) | OP(7) | OP(8), 4, 4 },
    /* 35 */ { { 0, OP(1), OP(2), 0, OP(4), 0, 0, 0 }, OP(3) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 36 */ { { 0, OP(1), OP(2), 0, 0, 0, 0, 0 }, OP(3) | OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 37 */ { { 0, 0, 0, OP(3), OP(4), OP(5), OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 38 */ { { 0, 0, 0, 0, OP(3) | OP(4), OP(5), OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 39 */ { { 0, 0, 0, 0, OP(4), OP(3) | OP(5), OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 40 */ { { 0, 0, 0, 0, OP(4), OP(5), OP(3) | OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 41 */ { { 0, 0, 0, 0, OP(4), OP(5), OP(6), OP(3) | OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 42 */ { { 0, 0, 0, 0, 0, OP(3) | OP(4) | OP(5), OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 43 */ { { 0, 0, 0, OP(3), 0, OP(5), OP(4) | OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 44 */ { { 0, 0, 0, 0, 0, OP(5), OP(3) | OP(4) | OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 45 */ { { 0, 0, 0, 0, OP(3) | OP(4), 0, OP(5) | OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 6, 6 },
    /* 46 */ { { 0, 0, 0, OP(3), 0, OP(5), OP(6), OP(4) | OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 47 */ { { 0, 0, 0, 0, OP(3) | OP(4), OP(5), 0, OP(6) | OP(7) }, OP(1) | OP(2) | OP(8), 7, 7 },
    /* 48 */ { { 0, 0, 0, 0, OP(4), OP(3) | OP(5), 0, OP(6) | OP(7) }, OP(1) | OP(2) | OP(8), 7, 7 },
    /* 49 */ { { 0, 0, 0, 0, 0, 0, OP(3) | OP(4) | OP(5) | OP(6), OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 50 */ { { 0, 0, 0, 0, 0, OP(3) | OP(4) | OP(5), 0, OP(6) | OP(7) }, OP(1) | OP(2) | OP(8), 7, 7 },
    /* 51 */ { { 0, 0, 0, 0, OP(3) | OP(4), 0, OP(6), OP(5) | OP(7) }, OP(1) | OP(2) | OP(8), 6, 6 },
    /* 52 */ { { 0, 0, 0, 0, 0, 0, OP(6), OP(3) | OP(4) | OP(5) | OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 53 */ { { 0, 0, 0, 0, OP(3) | OP(4), 0, 0, OP(5) | OP(6) | OP(7) }, OP(1) | OP(2) | OP(8), 7, 7 },
    /* 54 */ { { 0, 0, 0, 0, 0, 0, 0, OP(3) | OP(4) | OP(5) | OP(6) | OP(7) }, OP(1) | OP(2) | OP(8), 3, 3 },
    /* 55 */ { { 0, 0, 0, 0, OP(4), OP(5), OP(6), OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 1, 1 },
    /* 56 */ { { 0, 0, 0, 0, 0, OP(4) | OP(5), OP(6), OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 4, 4 },
    /* 57 */ { { 0, 0, 0, 0, 0, OP(5), OP(4) | OP(6), OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 4, 4 },
    /* 58 */ { { 0, 0, 0, 0, 0, OP(5), OP(6), OP(4) | OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 4, 4 },
    /* 59 */ { { 0, 0, 0, 0, 0, 0, OP(4) | OP(5) | OP(6), OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 4, 4 },
    /* 60 */ { { 0, 0, 0, 0, OP(4), 0, OP(6), OP(5) | OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 4, 4 },
    /* 61 */ { { 0, 0, 0, 0, 0, 0, OP(6), OP(4) | OP(5) | OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 4, 4 },
    /* 62 */ { { 0, 0, 0, 0, 0, OP(4) | OP(5), 0, OP(6) | OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 7, 7 },
    /* 63 */ { { 0, 0, 0, 0, 0, 0, 0, OP(4) | OP(5) | OP(6) | OP(7) }, OP(1) | OP(2) | OP(3) | OP(8), 4, 4 },
    /* 64 */ { { 0, 0, 0, 0, OP(3) | OP(4), 0, 0, OP(6) | OP(7) }, OP(1) | OP(2) | OP(5) | OP(8), 3, 3 },
    /* 65 */ { { 0, 0, 0, 0, OP(3) | OP(4), OP(3) | OP(4), OP(3) | OP(4), OP(3) | OP(4) }, OP(1) | OP(2) | OP(5) | OP(6) | OP(7) | OP(8), 3, 3 },
    /* 66 */ { { 0, OP(1), OP(2), OP(3), OP(4), OP(5), OP(6), OP(7) }, OP(8), 1, 1 },
    /* 67 */ { { 0, OP(1), 0, OP(3), 0, OP(5), 0, OP(7) }, OP(2) | OP(4) | OP(6) | OP(8), 1, 1 },
    /* 68 */ { { 0, 0, 0, 0, 0, 0, 0, OP(1) | OP(2) | OP(3) | OP(4) | OP(5) | OP(6) | OP(7) }, OP(8), 1, 1 },
    /* 69 */ { { 0, OP(1), OP(2), OP(3), 0, OP(5), OP(6), OP(7) }, OP(4) | OP(8), 1, 1 },
    /* 70 */ { { 0, OP(1), OP(2), OP(3), 0, 0, OP(5) | OP(6), OP(7) }, OP(4) | OP(8), 1, 1 },
    /* 71 */ { { 0, 0, OP(2), OP(1) | OP(3), 0, 0, OP(6), OP(5) | OP(7) }, OP(4) | OP(8), 1, 1 },
    /* 72 */ { { 0, OP(1), 0, OP(2) | OP(3), 0, OP(5), 0, OP(6) | OP(7) }, OP(4) | OP(8), 1, 1 },
    /* 73 */ { { 0, 0, OP(2), 0, OP(4), OP(4), OP(4), OP(4) }, OP(1) | OP(3) | OP(5) | OP(6) | OP(7) | OP(8), 2, 2 },
    /* 74 */ { { 0, OP(1), 0, OP(3), 0, OP(5), 0, OP(6) | OP(7) }, OP(2) | OP(4) | OP(8), 1, 1 },
    /* 75 */ { { 0, OP(1), OP(1), 0, OP(4), 0, OP(6), OP(5) | OP(7) }, OP(2) | OP(3) | OP(8), 1, 1 },
    /* 76 */ { { 0, OP(1), OP(2), 0, OP(4), OP(4), OP(4), OP(4) }, OP(3) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 77 */ { { 0, 0, 0, OP(1) | OP(2) | OP(3), 0, 0, 0, OP(5) | OP(6) | OP(7) }, OP(4) | OP(8), 1, 1 },
    /* 78 */ { { 0, 0, OP(1) | OP(2), 0, OP(4), 0, OP(6), 0 }, OP(3) | OP(5) | OP(7) | OP(8), 1, 1 },
    /* 79 */ { { 0, 0, OP(1) | OP(2), OP(3), 0, OP(5), OP(6), OP(7) }, OP(4) | OP(8), 1, 1 },
    /* 80 */ { { 0, 0, OP(1) | OP(2), OP(1) | OP(2), OP(1) | OP(2), OP(1) | OP(2), OP(1) | OP(2), OP(1) | OP(2) }, OP(3) | OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 81 */ { { 0, OP(1), 0, OP(3), OP(4), 0, OP(6), OP(7) }, OP(2) | OP(5) | OP(8), 1, 1 },
    /* 82 */ { { 0, OP(1), OP(2), OP(2), 0, OP(5), OP(6), OP(6) }, OP(3) | OP(4) | OP(7) | OP(8), 1, 1 },
    /* 83 */ { { 0, OP(1), 0, 0, OP(3) | OP(4), 0, 0, OP(6) | OP(7) }, OP(2) | OP(5) | OP(8), 1, 1 },
    /* 84 */ { { 0, OP(1), OP(1), OP(1), OP(1), 0, OP(6), OP(6) }, OP(2) | OP(3) | OP(4) | OP(5) | OP(7) | OP(8), 6, 6 },
    /* 85 */ { { 0, OP(1), 0, 0, OP(4), 0, OP(6), OP(3) | OP(5) | OP(7) }, OP(2) | OP(8), 1, 1 },
    /* 86 */ { { 0, OP(1), OP(2), OP(3), OP(3), OP(3), OP(3), OP(3) }, OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 87 */ { { 0, OP(1), OP(2), OP(2), OP(2), OP(2), OP(2), OP(2) }, OP(3) | OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
    /* 88 */ { { 0, 0, OP(1) | OP(2), OP(3), OP(3), OP(3), OP(3), OP(3) }, OP(4) | OP(5) | OP(6) | OP(7) | OP(8), 1, 1 },
};

#undef OP

// An algorithm prepared for rendering. Operators are addressed by their index in the
// voice (FmOpParams order): for n operators, operator k is index n - k, so a DX7 voice's
// operator 6 is index 0, as in DX7 SysEx.
struct CompiledAlgorithm {
    int count = 0;           // operators
    int8_t order[8] = {};    // render order: every operator after all its modulators
    uint8_t inputs[8] = {};  // per index: mask of the indices modulating it
    uint8_t outputs = 0;     // indices summed into the output
    int8_t feedbackOp = -1;  // index that takes feedback from feedbackFrom
    int8_t feedbackFrom = -1;
    int8_t loop[8] = {};     // feedbackOp ... feedbackFrom along the signal (length 1: self feedback)
    int loopLength = 0;
    uint8_t active = 0;      // indices whose envelopes keep a voice playing
    bool valid = false;      // false: the graph had a cycle in its inputs (feedback is separate)
};

// Prepares graph g of n operators (6 or 8); extraActive adds operators to `active`. A graph
// whose inputs form a cycle cannot be rendered in order: the result has valid == false.
inline CompiledAlgorithm compileAlgorithm(const OperatorGraph &g, int n, uint8_t extraActive = 0) {
    CompiledAlgorithm a;
    a.count = n;
    auto index = [n](int op) { return n - op; };
    auto maskToIndices = [&](uint8_t m) {
        uint8_t r = 0;
        for (int op = 1; op <= n; op++)
            if (m & (1u << (op - 1)))
                r |= (uint8_t)(1u << index(op));
        return r;
    };
    for (int op = 1; op <= n; op++)
        a.inputs[index(op)] = maskToIndices(g.inputs[op - 1]);
    a.outputs = maskToIndices(g.carriers);
    a.active = (uint8_t)(a.outputs | maskToIndices(extraActive));
    // topological order, lowest index first among the ready ones
    uint8_t done = 0;
    for (int k = 0; k < n; k++) {
        int next = -1;
        for (int i = 0; i < n && next < 0; i++)
            if (!(done & (1u << i)) && (a.inputs[i] & ~done) == 0)
                next = i;
        if (next < 0)
            return a; // a cycle: no operator left whose modulators are all rendered
        a.order[k] = (int8_t)next;
        done |= (uint8_t)(1u << next);
    }
    a.valid = true;
    if (g.feedbackTo != 0) {
        a.feedbackOp = (int8_t)index(g.feedbackTo);
        a.feedbackFrom = (int8_t)index(g.feedbackFrom);
        // follow the signal from feedbackTo to feedbackFrom
        int at = a.feedbackOp;
        a.loop[a.loopLength++] = (int8_t)at;
        while (at != a.feedbackFrom && a.loopLength < n) {
            int next = -1;
            for (int i = 0; i < n; i++)
                if (a.inputs[i] & (1u << at)) {
                    // the operator on the way to feedbackFrom
                    uint8_t reach = (uint8_t)(1u << i);
                    for (int r = 0; r < n; r++)
                        for (int j = 0; j < n; j++)
                            if (a.inputs[j] & reach)
                                reach |= (uint8_t)(1u << j);
                    if (i == a.feedbackFrom || (reach & (1u << a.feedbackFrom)))
                        next = i;
                }
            if (next < 0)
                break;
            at = next;
            a.loop[a.loopLength++] = (int8_t)at;
        }
    }
    return a;
}

// The 32 DX7 algorithms, compiled once.
inline const CompiledAlgorithm &dx7Compiled(int algorithm) {
    static const struct Table {
        CompiledAlgorithm a[32];
        Table() {
            for (int i = 0; i < 32; i++)
                a[i] = compileAlgorithm(dx7Algorithms[i], 6, dx7ExtraActive[i]);
        }
    } table;
    return table.a[algorithm];
}

// The 88 Extended algorithms, compiled once.
inline const CompiledAlgorithm &extCompiled(int algorithm) {
    static const struct Table {
        CompiledAlgorithm a[88];
        Table() {
            for (int i = 0; i < 88; i++)
                a[i] = compileAlgorithm(extAlgorithms[i], 8);
        }
    } table;
    return table.a[algorithm];
}

#endif
