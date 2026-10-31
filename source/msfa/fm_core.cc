/*
 * Copyright 2012 Google Inc.
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

#ifdef VERBOSE
#include <iostream>
#endif

#include "synth.h"
#include "exp2.h"
#include "fm_op_kernel.h"
#include "fm_core.h"


namespace {
    // The MSFA ("Modern") kernels.
    struct ModernKernels {
        bool gains(FmOpParams &p, int32_t &gain1, int32_t &gain2) {
            const int kLevelThresh = 1120;
            gain1 = p.gain_out;
            gain2 = Exp2::lookup(p.level_in - (14 * (1 << 24)));
            p.gain_out = gain2;
            return gain1 >= kLevelThresh || gain2 >= kLevelThresh;
        }
        void compute(int32_t *o, const int32_t *in, int32_t ph, int32_t f, int32_t g1, int32_t g2, bool add) {
            FmOpKernel::compute(o, in, ph, f, g1, g2, add);
        }
        void compute_pure(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, bool add) {
            FmOpKernel::compute_pure(o, ph, f, g1, g2, add);
        }
        void compute_fb(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *fb, int shift, bool add) {
            FmOpKernel::compute_fb(o, ph, f, g1, g2, fb, shift, add);
        }
        void compute_fb_mod(int32_t *o, const int32_t *in, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *fb, int shift) {
            FmOpKernel::compute_fb_mod(o, in, ph, f, g1, g2, fb, shift);
        }
        int selfFeedbackShift(int shift) { return shift; }
        // multi-operator loops: only their first operator feeds back on itself (as before)
        bool loop(const CompiledAlgorithm &, FmOpParams *, int32_t, int32_t, int32_t *, int,
                  AlignedBuf<int32_t, N> *, bool *, uint8_t &) { return false; }
    };
}

void FmCore::render(int32_t *output, FmOpParams *params, int algorithm, int32_t *fb_buf, int feedback_shift) {
    ModernKernels k;
    renderGraph(k, dx7Compiled(algorithm), output, params, fb_buf, feedback_shift, buf_, sum_.get());
}

bool FmCore::isCarrier(int algorithm, int op) {
    return (dx7Compiled(algorithm).active >> op) & 1;
}
