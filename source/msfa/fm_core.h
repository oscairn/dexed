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

#ifndef __FM_CORE_H
#define __FM_CORE_H

#include "aligned_buf.h"
#include "fm_op_kernel.h"
#include "synth.h"
#include "controllers.h"


#include "algorithms.h"

// Renders one voice through a compiled algorithm (docs/oscairn-plan.md, section 3.6). Each
// operator renders into its own buffer from the sum of its audible modulators; carriers add
// into output. K is an engine's kernel set:
//   bool gains(FmOpParams &, int32_t &gain1, int32_t &gain2)  sets gain_out, says if audible
//   compute, compute_pure, compute_fb, compute_fb_mod          the engine's operator kernels
//   int selfFeedbackShift(int shift)                           shift for single-operator feedback
//   bool loop(...)                                             renders a multi-operator feedback
//                                                              loop, or returns false
// Integer sums wrap (-fwrapv), so the order of summing gives the same bits as the DX7 bus
// renderers this replaced.
template <class K>
void renderGraph(K &k, const CompiledAlgorithm &a, int32_t *output, FmOpParams *params,
                 int32_t *fb_buf, int fb_shift, AlignedBuf<int32_t, N> *bufs, int32_t *sum) {
    bool audible[8] = {};
    uint8_t rendered = 0;
    for (int n = 0; n < a.count; n++) {
        const int op = a.order[n];
        if (rendered & (1u << op))
            continue; // rendered inside a feedback loop
        FmOpParams &p = params[op];
        int32_t gain1, gain2;
        if (k.gains(p, gain1, gain2)) {
            int32_t *out = bufs[op].get();
            const int32_t *in = nullptr;
            int live = 0;
            for (int i = 0; i < a.count; i++) {
                if (((a.inputs[op] >> i) & 1) && audible[i]) {
                    if (live == 0) {
                        in = bufs[i].get();
                    } else {
                        if (live == 1) {
                            for (int j = 0; j < N; j++)
                                sum[j] = in[j];
                            in = sum;
                        }
                        const int32_t *b = bufs[i].get();
                        for (int j = 0; j < N; j++)
                            sum[j] += b[j];
                    }
                    live++;
                }
            }
            if (live > 0 && op == a.feedbackOp && fb_shift < 16 && a.loopLength == 1) {
                // a modulated operator that feeds back on itself (8-operator graphs; no DX7
                // algorithm has one, so their path below is unchanged)
                k.compute_fb_mod(out, in, p.phase, p.freq, gain1, gain2, fb_buf, k.selfFeedbackShift(fb_shift));
            } else if (live > 0) {
                k.compute(out, in, p.phase, p.freq, gain1, gain2, false);
            } else if (op == a.feedbackOp && fb_shift < 16) {
                if (k.loop(a, params, gain1, gain2, fb_buf, fb_shift, bufs, audible, rendered))
                    continue;
                k.compute_fb(out, p.phase, p.freq, gain1, gain2, fb_buf, k.selfFeedbackShift(fb_shift), false);
            } else {
                k.compute_pure(out, p.phase, p.freq, gain1, gain2, false);
            }
            audible[op] = true;
        }
        p.phase += p.freq << LG_N;
        rendered |= (uint8_t)(1u << op);
    }
    for (int i = 0; i < a.count; i++) {
        if (((a.outputs >> i) & 1) && audible[i]) {
            const int32_t *b = bufs[i].get();
            for (int j = 0; j < N; j++)
                output[j] += b[j];
        }
    }
}

class FmCore {
public:
    virtual ~FmCore() {};
    // whether operator op's envelope keeps a voice playing (FmOpParams index)
    static bool isCarrier(int algorithm, int op);
    virtual void render(int32_t *output, FmOpParams *params, int algorithm, int32_t *fb_buf, int32_t feedback_gain);
protected:
    AlignedBuf<int32_t, N> buf_[8]; // one per operator
    AlignedBuf<int32_t, N> sum_;    // modulator sums
};

#endif  // __FM_CORE_H
