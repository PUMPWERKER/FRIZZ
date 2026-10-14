// The state-variable filter in DjFilter::Channel::Process is stmlib's Svf (dsp/filter.h):
//
// Copyright 2014 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.

/** @file DJFilter.h
 *  @brief One-knob DJ filter: lowpass below the centre, highpass above, the dry input at it,
 *  with resonance (#304).
 *
 *  One state-variable filter a channel (Zavalishin's topology-preserving SVF, stmlib's
 *  Svf), its lowpass or its highpass, as Beads' multitap_delay.h does its DJ filter. WAVE's
 *  ran a lowpass and a highpass in series a channel, each two resonant one-poles
 *  (BasicMMF), and its lowpass stayed in on the highpass side and at the centre with the
 *  resonance on it: a peak near 8kHz (+5dB at the default resonance). Here the centre is
 *  flat.
 *
 *  The knob keeps WAVE's law, measured on WAVE's filter: x = .01 + 2 knob (lowpass, to .98)
 *  or 1.9 knob - 1 (highpass, to .9), and g = x^3 / 2 (WAVE's one-pole coefficient halved,
 *  which is where its -3dB points lie), bent a little where the two part: within half a
 *  semitone of WAVE's -3dB point at the default resonance. The resonance knob is the
 *  damping, from two real poles (WAVE's at 0) to a 20dB peak (WAVE's 19-21dB). 12dB/oct
 *  both ways: WAVE's highpass was 6dB/oct at low resonance.
 *
 *  Retune works the coefficients out, every few samples (FxFilter.h); Run is the per-sample
 *  part. */

#pragma once
#include <math.h>
#include "daisysp.h"

// WAVE's DJFilter.h had this, and headers included after it (CardLend.h) lean on it
namespace chompi {}
using namespace chompi;

class DjFilter
{
  public:
    // The knob's centre: the dry input from kLpDry to kHpDry, the lowpass or the highpass
    // fading in either side of it, within WAVE's flat stretch (its lowpass open from kLpOpen,
    // its highpass in from kHpIn)
    static constexpr float kLpOpen = .485f, kLpDry = .495f;
    static constexpr float kHpDry = .505f, kHpIn = 1.f / 1.9f;
    // The highpass never lower than ~8Hz: at g 0 it would hold whatever it had as DC
    static constexpr float kHpMinG = 5.2e-4f;
    // Retune every kRetune samples; between them the blend glides, by at most kWetStep a
    // sample: all the way in ~2.7ms. A cutoff that jumps across the centre (the LFO's
    // division changed) fades the one side out to dry before the other comes in
    static const uint32_t kRetune = 8;
    static constexpr float kWetStep = 1.f / 128.f;

    void Init()
    {
        ch_[0] = ch_[1] = Channel();
        SetRes(.5f);
        Retune(.5f, -1.f, true);
    }

    /** The resonance knob, 0..1 (FxFilter.h keeps it below 1), as the damping (1/Q): 2, two
     *  real poles, to .1 at .99, a 20dB peak. It counts from the next Retune */
    void SetRes(float res) { damp_ = 2.f - 1.92f * res; }

    /** The knob, 0..1, for each channel (right below 0: the left's), every kRetune samples;
     *  snap: the blend at once, no glide (at Init) */
    void Retune(float left, float right, bool snap = false)
    {
        const Coeffs k = Coefficients(left);
        ch_[0].Set(k, snap);
        ch_[1].Set(right < 0.f ? k : Coefficients(right), snap);
    }

    inline void Run(float in_l, float in_r, float* out_l, float* out_r)
    {
        *out_l = ch_[0].Process(in_l);
        *out_r = ch_[1].Process(in_r);
    }

  private:
    struct Coeffs
    {
        float g, rg, h, wet;
        bool hp;
    };

    Coeffs Coefficients(float cutoff) const
    {
        Coeffs k;
        float g;
        k.hp = cutoff >= .5f;
        if (!k.hp)
        {
            const float x = fminf(.01f + 2.f * cutoff, .98f), x3 = x * x * x;
            g = .5f * x3 / (1.f - .64f * x3 * x);
            k.wet = daisysp::fclamp((kLpDry - cutoff) * (1.f / (kLpDry - kLpOpen)), 0.f, 1.f);
        }
        else
        {
            const float x = daisysp::fclamp(1.9f * cutoff - 1.f, 0.f, .9f), x2 = x * x;
            g = fmaxf(.5f * x2 * x * (1.f + x2 * (.3f - .21f * x2)), kHpMinG);
            k.wet = daisysp::fclamp((cutoff - kHpDry) * (1.f / (kHpIn - kHpDry)), 0.f, 1.f);
        }
        k.g = g;
        k.rg = damp_ + g;
        k.h = 1.f / (1.f + damp_ * g + g * g);
        return k;
    }

    struct Channel
    {
        float s1 = 0.f, s2 = 0.f;
        float g = 0.f, rg = 0.f, h = 1.f;
        float wet = 0.f, wet_step = 0.f, wet_end = 0.f; // the blend, gliding to wet_end
        bool hp = true;

        void Set(const Coeffs& k, bool snap)
        {
            wet = wet_end; // where the glide was going, exactly
            float target = k.wet;
            // the other side: first this one out to dry, on the coefficients it has
            if (k.hp != hp && wet > 0.f && !snap)
                target = 0.f;
            else
                hp = k.hp;
            if (k.hp == hp)
            {
                g = k.g;
                rg = k.rg;
                h = k.h;
            }
            const float most = kWetStep * kRetune;
            wet_end = snap ? target : wet + daisysp::fclamp(target - wet, -most, most);
            if (snap)
                wet = target;
            wet_step = (wet_end - wet) * (1.f / kRetune);
        }

        inline float Process(float in)
        {
            const float yh = (in - rg * s1 - s2) * h;
            const float yb = g * yh + s1;
            s1 = g * yh + yb;
            const float yl = g * yb + s2;
            s2 = g * yb + yl;
            const float out = in + wet * ((hp ? yh : yl) - in);
            wet += wet_step;
            return out;
        }
    };

    Channel ch_[2];
    float damp_;
};
