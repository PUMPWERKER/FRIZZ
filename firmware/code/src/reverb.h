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
//
// -----------------------------------------------------------------------------
//
// Reverb.

#ifndef RINGS_DSP_FX_REVERB_H_
#define RINGS_DSP_FX_REVERB_H_

#include "FrizzHot.h"
#include "fx_engine.h"

namespace daisysp {

class Reverb {
 public:

  Reverb() {}
  ~Reverb() {}

 void Init(float sample_rate) {
    engine_.Init(buffer_);
    engine_.SetLFOFrequency(LFO_1, 0.5f / sample_rate);
    engine_.SetLFOFrequency(LFO_2, 0.3f / sample_rate);
    lp_ = 0.7f;
    diffusion_ = 0.625f;
    // FRIZZ: the reverb lives in DTCMRAM, which the startup code doesn't zero, so clear the
    // buffer and every state Process() reads instead of relying on static zero-init
    engine_.Clear();
    input_gain_ = 0.f;
    reverb_time_ = 0.5f;
    lp_decay_1_ = 0.f;
    lp_decay_2_ = 0.f;
    freeze_ = 0.f;
    ap_1_ = 0.f;
    ap_2_ = 0.f;
  }
  
  FRIZZ_HOT void Process(float* left, float* right) {
    // This is the Griesinger topology described in the Dattorro paper
    // (4 AP diffusers on the input, then a loop of 2x 2AP+1Delay).
    // Modulation is applied in the loop of the first diffuser AP for additional
    // smearing; and to the two long delays for a slow shimmer/chorus effect.
    typedef E::Reserve<150,
      E::Reserve<214,
      E::Reserve<319,
      E::Reserve<527,
      E::Reserve<2182,
      E::Reserve<2690,
      E::Reserve<4501,
      E::Reserve<2525,
      E::Reserve<2197,
      E::Reserve<6312> > > > > > > > > > Memory;
    E::DelayLine<Memory, 0> ap1;
    E::DelayLine<Memory, 1> ap2;
    E::DelayLine<Memory, 2> ap3;
    E::DelayLine<Memory, 3> ap4;
    E::DelayLine<Memory, 4> dap1a;
    E::DelayLine<Memory, 5> dap1b;
    E::DelayLine<Memory, 6> del1;
    E::DelayLine<Memory, 7> dap2a;
    E::DelayLine<Memory, 8> dap2b;
    E::DelayLine<Memory, 9> del2;
    E::Context c;

    const float kap = diffusion_;
    // (FRIZZ: without Elements' freeze, which held the loop and shut the input: FxReverb.h's)
    const float krt = fclamp(reverb_time_, 0.f, 1.f);
    const float klp = fclamp(lp_, 0.f, 1.f);
    const float gain = input_gain_;

    float lp_1 = lp_decay_1_;
    float lp_2 = lp_decay_2_;

      float wet;
      float apout = 0.0f;
      engine_.Start(&c);

    // Smear AP1 inside the loop (FRIZZ: on, as in Clouds, Elements and Beads; Rings has it
    // commented out): ap1's length varies, 59 to 139 samples, so each hit is diffused a little
    // differently. Beads' 10 + 80 * its LFO, which runs 0..1; FRIZZ's runs -1..1, so it's
    // 50 + 40 here. Read as 10 +- 80, it would reach behind ap1, into what del2 wrote 0.36 s
    // before: a second feedback path, under which the frozen room grows to the clip
    c.Interpolate(ap1, 50.0f, LFO_1, 40.0f, 1.0f);
    c.Write(ap1, 100, 0.0f);

    c.Read(*left + *right, gain);

    // Diffuse through 4 allpasses.
    c.Read(ap1 TAIL, kap);
    c.WriteAllPass(ap1, -kap);
    c.Read(ap2 TAIL, kap);
    c.WriteAllPass(ap2, -kap);
    c.Read(ap3 TAIL, kap);
    c.WriteAllPass(ap3, -kap);
    c.Read(ap4 TAIL, kap);
    c.WriteAllPass(ap4, -kap);
    c.Write(apout);

    // Main reverb loop. del2 and del1 read 6261 + 50 and 4460 + 40 times Mutable's LFOs,
    // which run 0..1, so they end on each line's last sample; FRIZZ's run -1..1, so the same
    // span is the middle plus or minus half (#313: it was twice as deep, centred lower).
    // Frozen, they're allpass-interpolated (fx_engine.h), so the held room keeps its highs,
    // and the depth halves (#394): their phase errors compound, trip after trip, and at full
    // depth an 8kHz tone's pitch spread 6 cents in a minute, at half 3
    const float depth = 1.f - .5f * freeze_;
    // Frozen, the tank's writes round to the nearest step (#419): truncating, each took up to a
    // step off, and quiet held material fell to nothing within minutes. 0 rounds to 0, so a
    // room frozen silent stays silent (the input's diffusers truncate still: they decay)
    const float round = .5f * freeze_;
    c.Load(apout);
    c.Interpolate(del2, 6286.0f, LFO_2, 25.0f * depth, krt, ap_2_, freeze_);
    c.Lp(lp_1, klp);
    c.Read(dap1a TAIL, -kap);
    c.WriteAllPassRound(dap1a, kap, round);
    c.Read(dap1b TAIL, kap);
    c.WriteAllPassRound(dap1b, -kap, round);
    c.WriteRound(del1, 2.0f, round);
    c.Write(wet, 0.0f);

    *left = wet;

    c.Load(apout);
    c.Interpolate(del1, 4480.0f, LFO_1, 20.0f * depth, krt, ap_1_, freeze_);
    c.Lp(lp_2, klp);
    c.Read(dap2a TAIL, kap);
    c.WriteAllPassRound(dap2a, -kap, round);
    c.Read(dap2b TAIL, -kap);
    c.WriteAllPassRound(dap2b, kap, round);
    c.WriteRound(del2, 2.0f, round);
    c.Write(wet, 0.0f);

    *right = wet;
          
    lp_decay_1_ = lp_1;
    lp_decay_2_ = lp_2;
  }
  
  inline void SetInputGain(float input_gain) {
    input_gain_ = input_gain;
  }

  inline void SetTime(float reverb_time) {
    reverb_time_ = reverb_time;
  }
  
  inline void SetDiffusion(float diffusion) {
    diffusion_ = diffusion;
  }
  
  inline void SetLowpass(float lp) {
    lp_ = lp;
  }

  // FRIZZ (#394): how frozen the room is, 0..1, for the tank's reads. Under 1e-4 it's 0: the
  // freeze's slew, back to 0, would stall on a denormal above it and leave the allpass on
  inline void SetFreeze(float freeze) {
    freeze_ = freeze < 1e-4f ? 0.f : freeze;
  }
  
 private:
  typedef FxEngine<32768, FORMAT_16_BIT> E;
  E engine_;
  
  float input_gain_;
  float reverb_time_;
  float diffusion_;
  float lp_;
  
  float lp_decay_1_;
  float lp_decay_2_;

  float freeze_;
  float ap_1_; // the tank's allpass reads' last outputs, del1's and del2's
  float ap_2_;
  
  uint16_t  buffer_[32768];
};

}  // namespace daisysp

#endif  // RINGS_DSP_FX_REVERB_H_
