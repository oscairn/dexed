/*
 * Copyright (C) 2015-2017 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * The code is based on ppplay https://github.com/stohrendorf/ppplay and opl3
 * math documentation :
 * https://github.com/gtaylormb/opl3_fpga/blob/master/docs/opl3math/opl3math.pdf
 *
 */

#include "EngineMkI.h"
#include <cmath>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "msfa/sin.h"
#include "msfa/exp2.h"

#ifdef DEBUG
    #include "time.h"
    //#define MKIDEBUG
#endif

#ifdef _WIN32
#if _MSC_VER < 1800
    double log2(double n)  {  
        return log(n) / log(2.0);  
    }
    double round(double n) {
        return n < 0.0 ? ceil(n - 0.5) : floor(n + 0.5);
    }
#endif
    __declspec(align(16)) const int zeros[N] = {0};
#else
    const int32_t __attribute__ ((aligned(16))) zeros[N] = {0};
#endif

static const uint16_t NEGATIVE_BIT = 0x8000;
static const uint16_t ENV_BITDEPTH = 14;

static const uint16_t SINLOG_BITDEPTH = 10;
static const uint16_t SINLOG_TABLESIZE = 1<<SINLOG_BITDEPTH;
static uint16_t sinLogTable[SINLOG_TABLESIZE];

static const uint16_t SINEXP_BITDEPTH = 10;
static const uint16_t SINEXP_TABLESIZE = 1<<SINEXP_BITDEPTH;
static uint16_t sinExpTable[SINEXP_TABLESIZE];

const uint16_t ENV_MAX = 1<<ENV_BITDEPTH;

static inline uint16_t sinLog(uint16_t phi) {
    const uint16_t SINLOG_TABLEFILTER = SINLOG_TABLESIZE-1;
    const uint16_t index = (phi & SINLOG_TABLEFILTER);
    
    switch( ( phi & (SINLOG_TABLESIZE * 3) ) ) {
        case 0:
            return sinLogTable[index];
        case SINLOG_TABLESIZE:
            return sinLogTable[index ^ SINLOG_TABLEFILTER];
        case SINLOG_TABLESIZE * 2 :
            return sinLogTable[index] | NEGATIVE_BIT;
        default:
            return sinLogTable[index ^ SINLOG_TABLEFILTER] | NEGATIVE_BIT;
    }
}

EngineMkI::EngineMkI() {
    // the tables are shared by every instance: build them once, thread-safely
    static const bool tablesReady = [] {
    float bitReso = SINLOG_TABLESIZE;
    
    for(int i=0;i<SINLOG_TABLESIZE;i++) {
        float x1 = sin(((0.5+i)/bitReso) * M_PI/2.0);
        sinLogTable[i] = round(-1024 * log2(x1));
    }
    
    bitReso = SINEXP_TABLESIZE;
    for(int i=0;i<SINEXP_TABLESIZE;i++) {
        float x1 = (pow(2, float(i)/bitReso)-1) * 4096;
        sinExpTable[i] = round(x1);
    }
    return true;
    }();
    (void)tablesReady;
    
#ifdef MKIDEBUG
    char buffer[4096];
    int pos = 0;
    
    TRACE("****************************************");
    for(int i=0;i<SINLOG_TABLESIZE;i++) {
        pos += sprintf(buffer+pos, "%d ", sinLogTable[i]);
        if ( pos > 90 ) {
            TRACE("SINLOGTABLE: %s" ,buffer);
            buffer[0] = 0;
            pos = 0;
        }
    }
    TRACE("SINLOGTABLE: %s", buffer);
    buffer[0] = 0;
    pos = 0;
    TRACE("----------------------------------------");    
    for(int i=0;i<SINEXP_TABLESIZE;i++) {
        pos += sprintf(buffer+pos, "%d ", sinExpTable[i]);
        if ( pos > 90 ) {
            TRACE("SINEXTTABLE: %s" ,buffer);
            buffer[0] = 0;
            pos = 0;
        }
    }
    TRACE("SINEXTTABLE: %s", buffer);
    TRACE("****************************************");
#endif
}

inline int32_t mkiSin(int32_t phase, uint16_t env) {
    uint16_t expVal = sinLog(phase >> (22 - SINLOG_BITDEPTH)) + (env);
    //int16_t expValShow = expVal;
    
    const bool isSigned = expVal & NEGATIVE_BIT;
    expVal &= ~NEGATIVE_BIT;
    
    const uint16_t SINEXP_FILTER = 0x3FF;
    uint16_t result = 4096 + sinExpTable[( expVal & SINEXP_FILTER ) ^ SINEXP_FILTER];
    
    //uint16_t resultB4 = result;
    result >>= ( expVal >> 10 ); // exp
    
#ifdef MKIDEBUG
    if ( ( time(NULL) % 5 ) == 0 ) {
        if ( expValShow < 0 ) {
            expValShow = (expValShow + 0x7FFF) * -1;
        }
        //TRACE(",%d,%d,%d,%d,%d,%d", phase >> (22 - SINLOG_BITDEPTH), env, expValShow, ( expVal & SINEXP_FILTER ) ^ SINEXP_FILTER, resultB4, result);
    }
#endif
    
    if( isSigned )
        return (-result - 1) << 13;
    else
        return result << 13;
}

void EngineMkI::compute(int32_t *output, const int32_t *input,
                        int32_t phase0, int32_t freq,
                        int32_t gain1, int32_t gain2, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = mkiSin((phase+input[i]), gain);
        output[i] = y + adder[i];
        phase += freq;
    }
    
}

void EngineMkI::compute_pure(int32_t *output, int32_t phase0, int32_t freq,
                             int32_t gain1, int32_t gain2, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = mkiSin(phase , gain);
        output[i] = y + adder[i];
        phase += freq;
    }
}

void EngineMkI::compute_fb(int32_t *output, int32_t phase0, int32_t freq,
                           int32_t gain1, int32_t gain2,
                           int32_t *fb_buf, int fb_shift, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        y0 = y;
        y = mkiSin((phase+scaled_fb), gain);
        output[i] = y + adder[i];
        phase += freq;
    }
    
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

// exclusively used for ALGO 6 with feedback
void EngineMkI::compute_fb2(int32_t *output, FmOpParams *parms, int32_t gain01, int32_t gain02, int32_t *fb_buf, int fb_shift) {
    int32_t dgain[2];
    int32_t gain[2];
    int32_t phase[2];
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    
    phase[0] = parms[0].phase;
    phase[1] = parms[1].phase;

    parms[1].gain_out = (ENV_MAX-(parms[1].level_in >> (28-ENV_BITDEPTH)));

    gain[0] = gain01;
    gain[1] = parms[1].gain_out == 0 ? (ENV_MAX-1) : parms[1].gain_out;

    dgain[0] = (gain02 - gain01 + (N >> 1)) >> LG_N;
    dgain[1] = (parms[1].gain_out - (parms[1].gain_out == 0 ? (ENV_MAX-1) : parms[1].gain_out));
    
    for (int i = 0; i < N; i++) {
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        
        // op 0
        gain[0] += dgain[0];
        y0 = y;
        y = mkiSin(phase[0]+scaled_fb, gain[0]);
        phase[0] += parms[0].freq;
        
        // op 1
        gain[1] += dgain[1];
        y = mkiSin(phase[1]+y, gain[1]);
        phase[1] += parms[1].freq;
        
        output[i] = y;
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

// exclusively used for ALGO 4 with feedback
void EngineMkI::compute_fb3(int32_t *output, FmOpParams *parms, int32_t gain01, int32_t gain02, int32_t *fb_buf, int fb_shift) {
    int32_t dgain[3];
    int32_t gain[3];
    int32_t phase[3];
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    
    phase[0] = parms[0].phase;
    phase[1] = parms[1].phase;
    phase[2] = parms[2].phase;
    
    parms[1].gain_out = (ENV_MAX-(parms[1].level_in >> (28-ENV_BITDEPTH)));
    parms[2].gain_out = (ENV_MAX-(parms[2].level_in >> (28-ENV_BITDEPTH)));
    
    gain[0] = gain01;
    gain[1] = parms[1].gain_out == 0 ? (ENV_MAX-1) : parms[1].gain_out;
    gain[2] = parms[2].gain_out == 0 ? (ENV_MAX-1) : parms[2].gain_out;

    dgain[0] = (gain02 - gain01 + (N >> 1)) >> LG_N;
    dgain[1] = (parms[1].gain_out - (parms[1].gain_out == 0 ? (ENV_MAX-1) : parms[1].gain_out));
    dgain[2] = (parms[2].gain_out - (parms[2].gain_out == 0 ? (ENV_MAX-1) : parms[2].gain_out));
    
    
    for (int i = 0; i < N; i++) {
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        
        // op 0
        gain[0] += dgain[0];
        y0 = y;
        y = mkiSin(phase[0]+scaled_fb, gain[0]);
        phase[0] += parms[0].freq;
        
        // op 1
        gain[1] += dgain[1];
        y = mkiSin(phase[1]+y, gain[1]);
        phase[1] += parms[1].freq;
        
        // op 2
        gain[2] += dgain[2];
        y = mkiSin(phase[2]+y, gain[2]);
        phase[2] += parms[2].freq;
        
        output[i] = y;
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

// compute_fb with a modulation input (graphs where a modulated operator feeds back; none of
// the DX7 algorithms has one). Writes output (no add).
void EngineMkI::compute_fb_mod(int32_t *output, const int32_t *input, int32_t phase0, int32_t freq,
                            int32_t gain1, int32_t gain2, int32_t *fb_buf, int fb_shift) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];

    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        y0 = y;
        y = mkiSin((phase+input[i]+scaled_fb), gain);
        output[i] = y;
        phase += freq;
    }

    fb_buf[0] = y0;
    fb_buf[1] = y;
}

namespace {
    // The Mark I kernels, with its multi-operator feedback loops for algorithms 4 and 6 and
    // its stronger feedback shift for algorithm 32.
    struct MarkIKernels {
        EngineMkI &e;
        int algorithm;
        bool gains(FmOpParams &p, int32_t &gain1, int32_t &gain2) {
            const int kLevelThresh = ENV_MAX-100;
            gain1 = p.gain_out == 0 ? (ENV_MAX-1) : p.gain_out;
            gain2 = ENV_MAX-(p.level_in >> (28-ENV_BITDEPTH));
            p.gain_out = gain2;
            return gain1 <= kLevelThresh || gain2 <= kLevelThresh;
        }
        void compute(int32_t *o, const int32_t *in, int32_t ph, int32_t f, int32_t g1, int32_t g2, bool add) {
            e.compute(o, in, ph, f, g1, g2, add);
        }
        void compute_pure(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, bool add) {
            e.compute_pure(o, ph, f, g1, g2, add);
        }
        void compute_fb(int32_t *o, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *fb, int shift, bool add) {
            e.compute_fb(o, ph, f, g1, g2, fb, shift, add);
        }
        void compute_fb_mod(int32_t *o, const int32_t *in, int32_t ph, int32_t f, int32_t g1, int32_t g2, int32_t *fb, int shift) {
            e.compute_fb_mod(o, in, ph, f, g1, g2, fb, shift);
        }
        int selfFeedbackShift(int shift) { return algorithm == 31 ? min((shift+2), 16) : shift; }
        bool loop(const CompiledAlgorithm &a, FmOpParams *params, int32_t gain1, int32_t gain2, int32_t *fb_buf,
                  int shift, AlignedBuf<int32_t, N> *bufs, bool *audible, uint8_t &rendered) {
            // compute_fb2/3 take the loop's operators as consecutive params, first one first
            if (a.loopLength < 2 || a.loopLength > 3)
                return false;
            for (int i = 0; i < a.loopLength; i++)
                if (a.loop[i] != a.loop[0] + i)
                    return false;
            const int last = a.loop[a.loopLength - 1];
            if (a.loopLength == 3)
                e.compute_fb3(bufs[last].get(), params + a.loop[0], gain1, gain2, fb_buf, min((shift+2), 16));
            else
                e.compute_fb2(bufs[last].get(), params + a.loop[0], gain1, gain2, fb_buf, min((shift+2), 16));
            for (int i = 0; i < a.loopLength; i++) {
                FmOpParams &p = params[a.loop[i]];
                p.phase += p.freq << LG_N;
                rendered |= (uint8_t)(1u << a.loop[i]);
            }
            audible[last] = true;
            return true;
        }
    };
}

void EngineMkI::render(int32_t *output, FmOpParams *params, int algorithm, int32_t *fb_buf, int32_t feedback_shift) {
    MarkIKernels k { *this, algorithm };
    renderGraph(k, dx7Compiled(algorithm), output, params, fb_buf, feedback_shift, buf_, sum_.get());
}
