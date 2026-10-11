/** @file FxReverb.h
 *  @brief TEMPO's reverb as a send.
 */
#pragma once
#include "FxCommon.h"
#include "reverb.h"

namespace chompi
{

// The reverb's pre-delay, 2 x 16384 frames (> 250ms) in SDRAM. Weak, so a header can define
// it once for every file that includes it; ZeroSDRAM clears it at boot
static const size_t kPreDelayFrames = 16384;
__attribute__((weak)) float reverb_pre_delay_mem[2][kPreDelayFrames] DSY_SDRAM_BSS;

/** TEMPO's reverb (reverb.h, the Rings/Clouds Griesinger topology) as a send.
 *  Params: 0 decay, 1 diffusion, 2 tone (damping), 3 level; page 2: 4 freeze (Rings' freeze,
 *  by degrees: the input shut, the decay and the tone up to 1, so the room holds), 5
 *  pre-delay (0 to 250ms before the room, the SP-404MK2's PRE DELAY), 7 ducking (the room
 *  ducks under what goes in). Page 2's 6, Band, filters what goes in (FxChain.h). The
 *  pre-delay's buffer is in SDRAM (reverb_pre_delay_mem). */
class ReverbSend : public FxBase
{
public:
    enum Param
    {
        DECAY,
        DIFFUSION,
        TONE,
        LEVEL,
        FREEZE,
        PRE_DELAY,
        DUCKING = 7,
    };

    /** The reverb's 64KB buffer lives in DTCMRAM, so it's a separate static (chompi_main.cpp) */
    FX_ONCE void Init(float sample_rate, daisysp::Reverb* reverb)
    {
        reverb_ = reverb;
        sample_rate_ = sample_rate;
        reverb_->Init(sample_rate);
        reverb_->SetInputGain(.3f); // WAVE's / TAPE's input gain
        gate_.Init();
        level_.Reset(0.f);
        decay_.Reset(.5f);
        tone_.Reset(.7f);
        diffusion_.Reset(.625f);
        freeze_.Reset(0.f);
        pre_.Reset(0.f);
        duck_.Init();
        pre_pos_ = pre_filled_ = 0;
    }

    /** Feeds in_l / in_r into the reverb (while on) and adds its return to *out_l / *out_r */
    void Process(float in_l, float in_r, float* out_l, float* out_r)
    {
        const float gate = gate_.Process();
        const bool silent_in = gate_.Asleep();
        if (tail_.Sleeping(silent_in, kSleepSamples))
            return;
        const float level = level_.Process();
        // the freeze: the room held, decay and tone up to 1, the input shut
        const float freeze = freeze_.Process();
        const float decay = decay_.Process(), tone = tone_.Process();
        reverb_->SetTime(decay + freeze * (1.f - decay));
        reverb_->SetLowpass(tone + freeze * (1.f - tone));
        reverb_->SetDiffusion(diffusion_.Process());

        const float in_gain = gate * (1.f - freeze);
        float wl = in_l * in_gain;
        float wr = in_r * in_gain;
        // the pre-delay, only while it's up: writing SDRAM every sample would push the rest
        // out of the cache. What it hasn't written since it came up reads as silence
        const float pre = pre_.Process();
        if (pre > 0.f || pre_.target > 0.f)
        {
            float* const pl = reverb_pre_delay_mem[0];
            float* const pr = reverb_pre_delay_mem[1];
            pl[pre_pos_] = wl;
            pr[pre_pos_] = wr;
            if (pre_filled_ < kPreDelayFrames)
                pre_filled_++;
            const bool held = pre + 2.f < static_cast<float>(pre_filled_);
            wl = held ? ReadFrac(pl, kPreDelayFrames - 1, pre_pos_, pre) : 0.f;
            wr = held ? ReadFrac(pr, kPreDelayFrames - 1, pre_pos_, pre) : 0.f;
            pre_pos_ = (pre_pos_ + 1) & (kPreDelayFrames - 1);
        }
        else
            pre_filled_ = 0;
        reverb_->Process(&wl, &wr);
        // ducking, under what goes in (as heard: the key's fade on it)
        if (duck_.amount > 0.f)
        {
            const float g = duck_.Gain(in_l * gate, in_r * gate);
            wl *= g;
            wr *= g;
        }
        tail_.Track(silent_in, wl, wr);

        *out_l += wl * level;
        *out_r += wr * level;
    }

    /** Off and its tail rung out: nothing to add, and its meter isn't needed */
    inline bool Sleeping() const { return tail_.quiet >= kSleepSamples; }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case DECAY:
            // TAPE keeps reverb time within .05-.97; below .3 there's barely a tail
            decay_.target = .3f + val * .67f;
            break;
        case TONE:
            // lowpass coefficient in the loop: dark (.3) to open (1)
            tone_.target = .3f + val * .7f;
            break;
        case DIFFUSION:
            diffusion_.target = .3f + val * .45f;
            break;
        case LEVEL:
            level_.target = val;
            break;
        case FREEZE:
            freeze_.target = val;
            break;
        case PRE_DELAY:
            pre_.target = val * .25f * sample_rate_;
            break;
        case DUCKING:
            duck_.amount = val;
            break;
        default:
            break;
        }
    }

private:
    // 2s: far longer than the reverb's delay lines, so nothing under -120dB can come back
    static const uint32_t kSleepSamples = 96000;
    float sample_rate_ = 48000.f;
    daisysp::Reverb* reverb_;
    TailWatch tail_;
    Smoothed level_;
    Smoothed decay_;
    Smoothed tone_;
    Smoothed diffusion_;
    Smoothed freeze_;
    Smoothed pre_;    // the pre-delay, frames
    Ducker duck_;
    size_t pre_pos_ = 0;
    size_t pre_filled_ = 0; // frames written since the pre-delay came up
};

} // namespace chompi
