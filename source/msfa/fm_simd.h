/*
 * The operator kernels' NEON loop, shared by the engines (arm64).
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "synth.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#define FM_SIMD 1
#include <arm_neon.h>

// The operator kernels' loop for four samples a step: sin4(phase, gain) is the engine's sine for four
// lanes, written step for step like its scalar one. phase and gain advance as the scalar loops do (the
// gain is incremented before each sample); `input` may be null.
template <class Sin4>
static inline void fmBlockSimd(int32_t *output, const int32_t *input, int32_t phase0, int32_t freq,
                               int32_t gain1, int32_t dgain, bool add, Sin4 sin4) {
    const uint32_t lane[4] = { 0, 1, 2, 3 }, step[4] = { 1, 2, 3, 4 };
    uint32x4_t phase = vaddq_u32(vdupq_n_u32((uint32_t)phase0), vmulq_u32(vld1q_u32(lane), vdupq_n_u32((uint32_t)freq)));
    uint32x4_t gain = vaddq_u32(vdupq_n_u32((uint32_t)gain1), vmulq_u32(vld1q_u32(step), vdupq_n_u32((uint32_t)dgain)));
    const uint32x4_t dphase = vdupq_n_u32((uint32_t)freq * 4), dg = vdupq_n_u32((uint32_t)dgain * 4);
    for (int i = 0; i < N; i += 4) {
        int32x4_t p = vreinterpretq_s32_u32(phase);
        if (input != nullptr)
            p = vaddq_s32(p, vld1q_s32(input + i));
        int32x4_t y = sin4(p, vreinterpretq_s32_u32(gain));
        if (add)
            y = vaddq_s32(y, vld1q_s32(output + i));
        vst1q_s32(output + i, y);
        phase = vaddq_u32(phase, dphase);
        gain = vaddq_u32(gain, dg);
    }
}
#endif

