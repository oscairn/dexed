/*
 * Copyright 2016-2025 Pascal Gauthier.
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

#include <math.h>
#include <stdlib.h>
#include "synth.h"
#include "freqlut.h"
#include "exp2.h"
#include "controllers.h"
#include "fm_core.h"
#include "dx7note.h"
#include <iostream>
#include <cmath>
#include <algorithm>

const int FEEDBACK_BITDEPTH = 8;

const int32_t coarsemul[] = {
    -16777216, 0, 16777216, 26591258, 33554432, 38955489, 43368474, 47099600,
    50331648, 53182516, 55732705, 58039632, 60145690, 62083076, 63876816,
    65546747, 67108864, 68576247, 69959732, 71268397, 72509921, 73690858,
    74816848, 75892776, 76922906, 77910978, 78860292, 79773775, 80654032,
    81503396, 82323963, 83117622
};

int32_t logfreq_round2semi(int freq) {
  const int base = 50857777;  // (1 << 24) * (log(440) / log(2) - 69/12)
  const int step = (1 << 24) / 12;
  const int rem = (freq - base) % step;
  return freq - rem;
}

int32_t Dx7Note::osc_freq(int midinote, int mode, int coarse, int fine, int detune, int channel) {

    // TODO: pitch randomization
    int32_t logfreq;
    if (mode == 0) {
        if (tuning_state_->is_standard_tuning() && MTS_HasMaster(mtsClient)) {
            mtsFreq = MTS_NoteToFrequency(mtsClient, midinote, channel - 1);
            logfreq = log(mtsFreq) * mtsLogFreqToNoteLogFreq;
        }
        else {
            mtsFreq = 0;
            logfreq = tuning_state_->midinote_to_logfreq(midinote);
        }

        // could use more precision, closer enough for now. those numbers comes from my DX7
        double detuneRatio = 0.0209 * exp(-0.396 * (((float)logfreq)/(1<<24))) / 7;
        logfreq += detuneRatio * logfreq * (detune - 7);
        
        logfreq += coarsemul[coarse & 31];
        if (fine) {
            // (1 << 24) / log(2)
            logfreq += (int32_t)floor(24204406.323123 * log(1 + 0.01 * fine) + 0.5);
        }
        
        // // This was measured at 7.213Hz per count at 9600Hz, but the exact
        // // value is somewhat dependent on midinote. Close enough for now.
        // //logfreq += 12606 * (detune -7);
    } else {
        // ((1 << 24) * log(10) / log(2) * .01) << 3
        logfreq = (4458616 * ((coarse & 3) * 100 + fine)) >> 3;
        logfreq += detune > 7 ? 13457 * (detune - 7) : 0;
    }
    return logfreq;
}

const uint8_t velocity_data[64] = {
    0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
    166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
    205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
    233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
    252, 253, 254
};

// See "velocity" section of notes. Returns velocity delta in microsteps.
int ScaleVelocity(int velocity, int sensitivity) {
    int clamped_vel = max(0, min(127, velocity));
    int vel_value = velocity_data[clamped_vel >> 1] - 239;
    int scaled_vel = ((sensitivity * vel_value + 7) >> 3) << 4;
    return scaled_vel;
}

int ScaleRate(int midinote, int sensitivity) {
    int x = min(31, max(0, midinote / 3 - 7));
    int qratedelta = (sensitivity * x) >> 3;
#ifdef SUPER_PRECISE
    int rem = x & 7;
    if (sensitivity == 3 && rem == 3) {
        qratedelta -= 1;
    } else if (sensitivity == 7 && rem > 0 && rem < 4) {
        qratedelta += 1;
    }
#endif
    return qratedelta;
}

const uint8_t exp_scale_data[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47, 56, 66,
    80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250
};

int ScaleCurve(int group, int depth, int curve) {
    int scale;
    if (curve == 0 || curve == 3) {
        // linear
        scale = (group * depth * 329) >> 12;
    } else {
        // exponential
        int n_scale_data = sizeof(exp_scale_data);
        int raw_exp = exp_scale_data[min(group, n_scale_data - 1)];
        scale = (raw_exp * depth * 329) >> 15;
    }
    if (curve < 2) {
        scale = -scale;
    }
    return scale;
}

int ScaleLevel(int midinote, int break_pt, int left_depth, int right_depth,
               int left_curve, int right_curve) {
    int offset = midinote - break_pt - 17;
    if (offset >= 0) {
        return ScaleCurve((offset+1) / 3, right_depth, right_curve);
    } else {
        return ScaleCurve(-(offset-1) / 3, left_depth, left_curve);
    }
}

static const uint8_t pitchmodsenstab[] = {
    0, 10, 20, 33, 55, 92, 153, 255
};

// 0, 66, 109, 255
static const uint32_t ampmodsenstab[] = {
    0, 4342338, 7171437, 16777216
};

const int32_t Dx7Note::mtsLogFreqToNoteLogFreq = (1 << 24) / log(2.);

Dx7Note::Dx7Note(std::shared_ptr<TuningState> ts, MTSClient *mtsc, const SynthRate *rate)
: tuning_state_(ts), mtsClient(mtsc), rate_(rate) {
    initialised_ = false;
    for(int op=0;op<6;op++) {
        params_[op].phase = 0;
        params_[op].gain_out = 0;
        env_[op].sr_multiplier = rate->envMultiplier;
    }
    pitchenv_.unit_ = rate->pitchEnvUnit;
    // the feedback history was read before ever being written
    fb_buf_[0] = 0;
    fb_buf_[1] = 0;
}

void Dx7Note::init(const uint8_t patch[156], int midinote, int velocity, int channel, const Controllers *ctrls,
                   const ExtendedVoice *ext, const ExtStart &start) {
    initialised_ = true;
    extended_ = ext != nullptr && ext->extended();
    if (extended_) {
        bendUpper_ = std::clamp(ext->common(ext::BendUpper), -127, 127);
        bendLower_ = std::clamp(ext->common(ext::BendLower), -127, 127);
        currentPatch = patch;
        playingMidiNote = midinote;
        midiChannel = channel;
        mpePitchBend = 8192;
        ext_.init(*ext, midinote, velocity, rate_, *tuning_state_, start);
        return;
    }
    currentPatch = patch;
    int rates[4];
    int levels[4];
    playingMidiNote = midinote;
    midiChannel = channel;

    for (int op = 0; op < 6; op++) {
        int off = op * 21;
        for (int i = 0; i < 4; i++) {
            rates[i] = patch[off + i];
            levels[i] = patch[off + 4 + i];
        }
        int outlevel = patch[off + 16];
        outlevel = Env::scaleoutlevel(outlevel);
        int level_scaling = ScaleLevel(midinote, patch[off + 8], patch[off + 9],
                                       patch[off + 10], patch[off + 11], patch[off + 12]);
        outlevel += level_scaling;
        outlevel = min(127, outlevel);
        outlevel = outlevel << 5;
        outlevel += ScaleVelocity(velocity, patch[off + 15]);
        outlevel = max(0, outlevel);
        int rate_scaling = ScaleRate(midinote, patch[off + 13]);
        env_[op].init(rates, levels, outlevel, rate_scaling);
        
        int mode = patch[off + 17];
        int coarse = patch[off + 18];
        int fine = patch[off + 19];
        int detune = patch[off + 20];
        int32_t freq = osc_freq(midinote, mode, coarse, fine, detune, channel);
        opMode[op] = mode;
        basepitch_[op] = freq;
        porta_curpitch_[op] = freq;
        ampmodsens_[op] = ampmodsenstab[patch[off + 14] & 3];
    }
    for (int i = 0; i < 4; i++) {
        rates[i] = patch[126 + i];
        levels[i] = patch[130 + i];
    }
    pitchenv_.set(rates, levels);
    algorithm_ = patch[134] > 31 ? 31 : patch[134]; // a corrupt voice must not index past the 32
    int feedback = patch[135];
    fb_shift_ = feedback != 0 ? FEEDBACK_BITDEPTH - feedback : 16;
    pitchmoddepth_ = (patch[139] * 165) >> 6;
    pitchmodsens_ = pitchmodsenstab[patch[143] & 7];
    ampmoddepth_ = (patch[140] * 165) >> 6;
    opSwitch_ = patch[155];

    // MPE default values
    mpePitchBend = 8192;
}

void Dx7Note::initPortamento(const Dx7Note &srcNote) {
    if (extended_)
        return; // shortcut: no portamento for Extended sounds yet (milestone M7)
    for (int i=0;i<6;i++) {
        porta_curpitch_[i] = srcNote.porta_curpitch_[i];
    }
}

// Pitch bend (with MPE and tuning) and master tune, in Q24 log frequency.
int32_t Dx7Note::bendAndTune(const Controllers *ctrls) {
    int pitchbend = ctrls->pitch_cc;
    int32_t pb = (pitchbend - 0x2000);
    if (pb != 0 && extended_ && ctrls->pitch_step == 0) {
        // the part's signed ranges, on the reference's pitch grid (extBendSteps)
        pb = extBendSteps(pb, pb >= 0 ? bendUpper_ : -bendLower_) << 13;
    } else if (pb != 0) {
        if (ctrls->pitch_step == 0) {
            if( pb >= 0 )
                pb = ((float) (pb << 11)) * ((float) ctrls->pitch_range_up) / 12.0;
            else
                pb = ((float) (pb << 11)) * ((float) ctrls->pitch_range_dn) / 12.0;
        } else {
            int stp = 12 / ctrls->pitch_step;
            pb = pb * stp / 8191;
            pb = (pb * (8191 / stp)) << 11;
        }
    }

    if( ctrls->mpeEnabled )
    {
        int d = ((float)( (mpePitchBend-0x2000) << 11 )) * ctrls->mpePitchBendRange / 12.0; 
        // std::cout << mpePitchBend << " " << 0x2000 << " " << d << std::endl;
        pb += d;
    }

    if( ! tuning_state_->is_standard_tuning() && pb != 0 )
    {
        // If we have a scale we want PB to be in scale space so we sort of need to
        // unwind the combinations above and re-interpolate
        
        float notesTuned = ( pb >> 11 ) * 12.0 / 8192; // How many steps you tuned
        int floorNote = std::floor(notesTuned);
        float frac = notesTuned - floorNote;
        float targetLog = tuning_state_->midinote_to_logfreq(playingMidiNote + floorNote) * ( 1.0 - frac ) +
            tuning_state_->midinote_to_logfreq(playingMidiNote + floorNote + 1) * frac; // the interpolated log freq
        float newpb = targetLog - tuning_state_->midinote_to_logfreq(playingMidiNote); // and the resulting bend
        pb = newpb;
    }
    
    return pb + ctrls->masterTune;
}

void Dx7Note::compute(int32_t *buf, int32_t lfo_val, int32_t lfo_delay, const Controllers *ctrls,
                      const float *part_lfo, int lfoSpeedSteps) {
    if (extended_) {
        ext_.compute(buf, bendAndTune(ctrls), ctrls, part_lfo, lfoSpeedSteps);
        return;
    }
    // ==== PITCH ====
    uint32_t pmd = pitchmoddepth_ * lfo_delay;  // Q32
    int32_t senslfo = pitchmodsens_ * (lfo_val - (1 << 23));
    int32_t pmod_1 = (((int64_t) pmd) * (int64_t) senslfo) >> 39;
    pmod_1 = abs(pmod_1);
    int32_t pmod_2 = (int32_t)(((int64_t)ctrls->pitch_mod * (int64_t)senslfo) >> 14);
    pmod_2 = abs(pmod_2);
    int32_t pitch_mod = max(pmod_1, pmod_2);
    pitch_mod = pitchenv_.getsample() + (pitch_mod * (senslfo < 0 ? -1 : 1));
    
    int32_t pitch_base = bendAndTune(ctrls);
    pitch_mod += pitch_base;
    
    // ==== AMP MOD ====
    lfo_val = (1<<24) - lfo_val;
    uint32_t amod_1 = (uint32_t)(((int64_t) ampmoddepth_ * (int64_t) lfo_delay) >> 8); // Q24 :D
    amod_1 = (uint32_t)(((int64_t) amod_1 * (int64_t) lfo_val) >> 24);
    uint32_t amod_2 = (uint32_t)(((int64_t) ctrls->amp_mod * (int64_t) lfo_val) >> 7); // Q?? :|
    uint32_t amd_mod = max(amod_1, amod_2);
    
    // ==== EG AMP MOD ====
    uint32_t amod_3 = (ctrls->eg_mod+1) << 17;
    amd_mod = max((1<<24) - amod_3, amd_mod);

    int porta_rate;
    if ( ctrls->portamento_enable_cc ) {
        if ( ctrls->portamento_gliss_cc )
            porta_rate = rate_->porta.rates_glissando[ctrls->portamento_cc];
        else
            porta_rate = rate_->porta.rates[ctrls->portamento_cc];
    } else {
        porta_rate = rate_->porta.rates[0];
    }

    // ==== OP RENDER ====
    for (int op = 0; op < 6; op++) {
        if ( ! (opSwitch_ & (1U << op) ) ) {
            env_[op].getsample(); // advance the envelop even if it is not playing
            params_[op].level_in = 0;
        } else {
            int32_t basepitch = basepitch_[op];

            if ( opMode[op] ) { 
                params_[op].freq = rate_->freqlut.lookup(basepitch + pitch_base);
            } else {
                if (porta_curpitch_[op] != basepitch_[op]) {
                    basepitch = porta_curpitch_[op];
                    if (ctrls->portamento_gliss_cc)
                        basepitch = logfreq_round2semi(basepitch);

                    int32_t cur = porta_curpitch_[op];
                    int32_t dst = basepitch_[op];

                    bool going_up = cur < dst;
                    int32_t newpitch = cur + (going_up ? +porta_rate : -porta_rate);

                    // Clamp to destination if we would overshoot/undershoot
                    if ((going_up && newpitch > dst) || (!going_up && newpitch < dst))
                        newpitch = dst;

                    porta_curpitch_[op] = newpitch;
                }
                params_[op].freq = rate_->freqlut.lookup(basepitch + pitch_mod);
            }

            int32_t level = env_[op].getsample();
            if (ampmodsens_[op] != 0) {
                uint32_t sensamp = (uint32_t)(((uint64_t) amd_mod) * ((uint64_t) ampmodsens_[op]) >> 24);
                
                // TODO: mehhh.. this needs some real tuning.
                uint32_t pt = exp(((float)sensamp)/262144 * 0.07 + 12.2);
                uint32_t ldiff = (uint32_t)(((uint64_t)level) * (((uint64_t)pt<<4)) >> 28);
                level -= ldiff;
            }
            params_[op].level_in = level;
        }
    }
    ctrls->core->render(buf, params_, algorithm_, fb_buf_, fb_shift_);
}

void Dx7Note::keyup() {
    if (extended_) {
        ext_.keyup();
        return;
    }
    for (int op = 0; op < 6; op++) {
        env_[op].keydown(false);
    }
    pitchenv_.keydown(false);
}

void Dx7Note::updateBasePitches()
{
    if (extended_)
        return; // Extended sounds use standard tuning in M4
    double f = MTS_NoteToFrequency(mtsClient, playingMidiNote, midiChannel - 1);
    if (f == mtsFreq) return;
    mtsFreq = f;
    //noteLogFreq = log(mtsFreq) * mtsLogFreqToNoteLogFreq;
    for (int op = 0; op < 6; op++)
    {
        int off = op * 21;
        int mode = currentPatch[off + 17];
        int coarse = currentPatch[off + 18];
        int fine = currentPatch[off + 19];
        int detune = currentPatch[off + 20];
        basepitch_[op] = osc_freq(playingMidiNote, mode, coarse, fine, detune, midiChannel);
    }
}

void Dx7Note::update(const uint8_t patch[156], int midinote, int velocity, int channel, const ExtendedVoice *ext) {
    if (extended_) {
        if (ext != nullptr && ext->extended()) {
            bendUpper_ = std::clamp(ext->common(ext::BendUpper), -127, 127);
            bendLower_ = std::clamp(ext->common(ext::BendLower), -127, 127);
            ext_.update(*ext, midinote, velocity, rate_, *tuning_state_);
        }
        return;
    }
    currentPatch = patch;
    int rates[4];
    int levels[4];
    playingMidiNote = midinote;
    midiChannel = channel;
    
    for (int op = 0; op < 6; op++) {
        int off = op * 21;
        int mode = patch[off + 17];
        int coarse = patch[off + 18];
        int fine = patch[off + 19];
        int detune = patch[off + 20];
        basepitch_[op] = osc_freq(midinote, mode, coarse, fine, detune, channel);
        ampmodsens_[op] = ampmodsenstab[patch[off + 14] & 3];
        opMode[op] = mode;
        
        for (int i = 0; i < 4; i++) {
            rates[i] = patch[off + i];
            levels[i] = patch[off + 4 + i];
        }
        int outlevel = patch[off + 16];
        outlevel = Env::scaleoutlevel(outlevel);
        int level_scaling = ScaleLevel(midinote, patch[off + 8], patch[off + 9],
                                       patch[off + 10], patch[off + 11], patch[off + 12]);
        outlevel += level_scaling;
        outlevel = min(127, outlevel);
        outlevel = outlevel << 5;
        outlevel += ScaleVelocity(velocity, patch[off + 15]);
        outlevel = max(0, outlevel);
        int rate_scaling = ScaleRate(midinote, patch[off + 13]);
        env_[op].update(rates, levels, outlevel, rate_scaling);
    }
    algorithm_ = patch[134] > 31 ? 31 : patch[134];
    int feedback = patch[135];
    fb_shift_ = feedback != 0 ? FEEDBACK_BITDEPTH - feedback : 16;
    pitchmoddepth_ = (patch[139] * 165) >> 6;
    pitchmodsens_ = pitchmodsenstab[patch[143] & 7];
    ampmoddepth_ = (patch[140] * 165) >> 6;
    opSwitch_ = patch[155];
}

void Dx7Note::peekVoiceStatus(VoiceStatus &status) {
    if (extended_) {
        status = VoiceStatus {};
        return;
    }
    for(int i=0;i<6;i++) {
        status.amp[i] = Exp2::lookup(params_[i].level_in - (14 * (1 << 24)));
        env_[i].getPosition(&status.ampStep[i]);
    }
    pitchenv_.getPosition(&status.pitchStep);
}

/**
 * Used in monophonic mode to transfer voice state from different notes
 */
void Dx7Note::transferState(Dx7Note &src) {
    if (extended_ || src.extended_) {
        if (extended_ && src.extended_)
            ext_.transferState(src.ext_);
        return;
    }
    for (int i=0;i<6;i++) {
        env_[i].transfer(src.env_[i]);
        params_[i].gain_out = src.params_[i].gain_out;
        params_[i].phase = src.params_[i].phase;
    }
}

void Dx7Note::transferSignal(Dx7Note &src) {
    if (extended_ || src.extended_) {
        if (extended_ && src.extended_)
            ext_.transferSignal(src.ext_);
        return;
    }
    for (int i=0;i<6;i++) {
        params_[i].gain_out = src.params_[i].gain_out;
        params_[i].phase = src.params_[i].phase;
    }
}

void Dx7Note::transferPhase(Dx7Note &src) {
    if (extended_ || src.extended_) {
        if (extended_ && src.extended_)
            ext_.transferPhase(src.ext_);
        return;
    }
    for (int i=0;i<6;i++) {
        params_[i].phase = src.params_[i].phase;
    }
}

void Dx7Note::oscSync() {
    if (extended_) {
        ext_.oscSync();
        return;
    }
    for (int i=0;i<6;i++) {
        params_[i].gain_out = 0;
        params_[i].phase = 0;
    }
}

double Dx7Note::carrierGain() const {
    if (extended_)
        return ext_.carrierGain();
    int32_t most = 0;
    for (int i = 0; i < 6; i++)
        if ((dx7Compiled(algorithm_).outputs >> i) & 1)
            most = std::max(most, params_[i].level_in);
    // level 99 at velocity 127 comes to about 15 << 24
    return Exp2::lookup(std::min(most, 20 << 24) - (15 << 24)) / (double)(1 << 24);
}

// a note is playing if it's been initialised and any carrier's amp
// envelope is active
bool Dx7Note::isPlaying() {
    if (extended_)
        return initialised_ && ext_.isPlaying();
    if ( !initialised_ ) return false;
    for (int i=0; i<6; i++) {
        if ( FmCore::isCarrier(algorithm_, i) && env_[i].isActive() ) {
            return true;
        }
    }
    return false;
}
