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

#undef OP

#endif
