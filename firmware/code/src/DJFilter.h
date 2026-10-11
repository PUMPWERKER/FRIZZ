// One-knob DJ filter: low-pass below center, high-pass above, with resonance.
// Derived from Electrosmith DSP source.

#pragma once
#include "BasicMMF.h"

using namespace chompi;

class DjFilter
{
  public:
    void Init(float samplerate)
    {
        feedback_filt_llp_.Init(samplerate);
        feedback_filt_rlp_.Init(samplerate);
        feedback_filt_lhp_.Init(samplerate);
        feedback_filt_rhp_.Init(samplerate);

        feedback_filt_llp_.SetMode(BasicMMF::Mode::Lowpass);
        feedback_filt_rlp_.SetMode(BasicMMF::Mode::Lowpass);
        feedback_filt_lhp_.SetMode(BasicMMF::Mode::Highpass);
        feedback_filt_rhp_.SetMode(BasicMMF::Mode::Highpass);

        feedback_filt_llp_.SetFreq(.99f);
        feedback_filt_rlp_.SetFreq(.99f);
        feedback_filt_lhp_.SetFreq(0.f);
        feedback_filt_rhp_.SetFreq(0.f);

        feedback_filt_llp_.SetRes(.6f);
        feedback_filt_rlp_.SetRes(.6f);
        feedback_filt_lhp_.SetRes(.6f);
        feedback_filt_rhp_.SetRes(.6f);

        // FRIZZ: start at the centre (no filtering), without a sweep from 0
        slew_ = .0002f;
        SetControl(.5f);
        lp_ = lp_target_;
        hp_ = hp_target_;
    }

    /** FRIZZ: how fast the cutoff follows SetControl(). WAVE's .0002 (~100ms) is too slow
     *  for an LFO, so FRIZZ slews the knob itself and sets 1 (no slew). */
    void SetSlew(float coeff) { slew_ = coeff; }
    
    void Process(float in_l, float in_r, float *out_l, float* out_r)
    {
        Retune();
        Run(in_l, in_r, out_l, out_r);
    }

    /** FRIZZ: Process in two: the stages' settings from SetControl, then the stages. Asleep
     *  (FxFilter.h), the settings are worked out only now and then */
    inline void Retune()
    {
        daisysp::fonepole(lp_, lp_target_, slew_);
        daisysp::fonepole(hp_, hp_target_, slew_);

        // the right channel's filters are the left's: their division (CalculateFeedback) once.
        // FRIZZ: unless it has a cutoff of its own (SetControlR), the stereo LFO
        feedback_filt_llp_.SetFreq(lp_);
        const bool own_r = lp_r_target_ >= 0.f;
        if (own_r)
        {
            daisysp::fonepole(lp_r_, lp_r_target_, slew_);
            daisysp::fonepole(hp_r_, hp_r_target_, slew_);
            feedback_filt_rlp_.SetFreq(lp_r_);
            feedback_filt_rhp_.SetFreq(fmaxf(hp_r_, 1e-3f));
        }
        else
        {
            lp_r_ = lp_;
            hp_r_ = hp_;
            feedback_filt_rlp_.CopySettings(feedback_filt_llp_);
        }

        // FRIZZ: never let the highpass reach exactly 0. Without WAVE's slew, hp_ lands on 0
        // as the cutoff crosses the centre, the highpass state stops updating, and whatever
        // it held stays in the output as DC. 1e-3 is a ~8Hz highpass that drains it in ~20ms.
        const float hp = fmaxf(hp_, 1e-3f);
        feedback_filt_lhp_.SetFreq(hp);
        if (!own_r)
            feedback_filt_rhp_.CopySettings(feedback_filt_lhp_);
        // (WAVE raised the highpass's resonance above hp_ .8; hp_ tops out at .9^3 = .73)
    }

    inline void Run(float in_l, float in_r, float *out_l, float* out_r)
    {
        float filt_l = feedback_filt_llp_.Process(in_l);
        float filt_r = feedback_filt_rlp_.Process(in_r);
        
        filt_l = feedback_filt_lhp_.Process(filt_l);
        filt_r = feedback_filt_rhp_.Process(filt_r);
        
        *out_l = filt_l;
        *out_r = filt_r;
    }

    void SetControl(float cutoff) { Targets(cutoff, &lp_target_, &hp_target_); }

    /** FRIZZ: the right channel's cutoff on its own, as SetControl; below 0: the left's */
    void SetControlR(float cutoff)
    {
        if (cutoff < 0.f)
        {
            lp_r_target_ = -1.f;
            return;
        }
        Targets(cutoff, &lp_r_target_, &hp_r_target_);
    }

    /** FRIZZ: the knob, 0..1, as the lowpass's and the highpass's frequencies */
    static inline void Targets(float cutoff, float* lp, float* hp)
    {
        const float l = daisysp::fclamp(.01f + cutoff * 2.f, 0.f, .98f); //these have to be limited
        *lp = l * l * l;
        const float h = daisysp::fclamp((cutoff * 1.9f) - 1.f, 0.f, .9f);
        *hp = h * h * h;
    }

    void SetRes(float res) 
    {
        res *= .95f;
        feedback_filt_llp_.SetRes(res);
        feedback_filt_rlp_.SetRes(res);

        feedback_filt_lhp_.SetRes(res);
        feedback_filt_rhp_.SetRes(res);
    }

    BasicMMF   feedback_filt_llp_, feedback_filt_rlp_;
    BasicMMF   feedback_filt_lhp_, feedback_filt_rhp_;
    float slew_;
    float lp_, lp_target_;
    float hp_, hp_target_;
    float lp_r_ = 0.f, lp_r_target_ = -1.f; // FRIZZ: the right channel's own, SetControlR
    float hp_r_ = 0.f, hp_r_target_ = 0.f;
};