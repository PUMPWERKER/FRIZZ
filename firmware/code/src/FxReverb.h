/** @file FxReverb.h
 *  @brief TEMPO's reverb as a send.
 */
#pragma once
#include "FxCommon.h"
#include "FxOutput.h"
#include "reverb.h"

namespace chompi
{

// The reverb's pre-delay, 2 x 16384 frames (> 250ms) in SDRAM. Weak, so a header can define
// it once for every file that includes it; ZeroSDRAM clears it at boot. FxChain.cpp only
// declares it (FRIZZ_FXCHAIN_CPP, #286): in a section of its own, ld would keep a copy for
// each object, one of them unused
static const size_t kPreDelayFrames = 16384;
#if FRIZZ_FXCHAIN_CPP
extern float reverb_pre_delay_mem[2][kPreDelayFrames];
#else
__attribute__((weak)) float reverb_pre_delay_mem[2][kPreDelayFrames] DSY_SDRAM_BSS;
#endif

/** TEMPO's reverb (reverb.h, the Rings/Clouds Griesinger topology) as a send.
 *  Params: 0 decay, 1 diffusion, 2 tone (damping), 3 ducking (the room ducks under what goes
 *  in); page 2: 4 freeze (Elements' freeze, by degrees: the input shut, the decay and the tone
 *  up to 1, so the room holds), 5 pre-delay (0 to 250ms before the room, the SP-404MK2's PRE
 *  DELAY), 7 level, its return's, on the inserts' Level scale (FxOutput::LevelGain), on page
 *  2's knob 4 as theirs (#172). Page 2's 6, Band, filters what goes in (FxChain.h). The
 *  pre-delay's buffer is in SDRAM (reverb_pre_delay_mem). */
class ReverbSend : public FxBase
{
public:
    enum Param
    {
        DECAY,
        DIFFUSION,
        TONE,
        DUCKING,
        FREEZE,
        PRE_DELAY,
        LEVEL = FxOutput::kLevel,
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
        pre_fade_ = 0;
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
        reverb_->SetFreeze(freeze);
        reverb_->SetDiffusion(diffusion_.Process());

        const float in_gain = gate * (1.f - freeze);
        float wl = in_l * in_gain;
        float wr = in_r * in_gain;
        // the pre-delay, only while it's up: writing SDRAM every sample would push the rest
        // out of the cache. What it hasn't written since it came up reads as silence. Turned,
        // it crossfades to the new time (PreFade): sliding there dragged the read head
        // through the buffer, up to 12 frames a sample (48 after a recall), a sped-up or
        // reversed burst into the room, a chirp. It lands on the target, so turned back to 0
        // it switches off
        if (PreDelaying())
        {
            float* const pl = reverb_pre_delay_mem[0];
            float* const pr = reverb_pre_delay_mem[1];
            pl[pre_pos_] = wl;
            pr[pre_pos_] = wr;
            if (pre_filled_ < kPreDelayFrames)
                pre_filled_++;
            if (pre_.value != pre_.target && !pre_fade_)
                StartPreFade();
            wl = PreTap(pl, pre_.value);
            wr = PreTap(pr, pre_.value);
            if (pre_fade_)
            {
                wl = PreFade(pl, wl);
                wr = PreFade(pr, wr);
                pre_fade_--;
            }
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
    /** The pre-delay up, or still fading down: its buffer written and read */
    inline bool PreDelaying() const { return pre_.value > 0.f || pre_.target > 0.f || pre_fade_; }

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
            level_.target = FxOutput::LevelGain(val);
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
    static const uint32_t kPreFadeFrames = 1024; // 21ms, as the delay's events'

    /** The pre-delay's buffer `pre` frames back, silence where it hasn't written yet */
    float PreTap(const float* buf, float pre) const
    {
        return pre + 2.f < static_cast<float>(pre_filled_) ? ReadFrac(buf, kPreDelayFrames - 1, pre_pos_, pre)
                                                           : 0.f;
    }
    /** The knob moved: the read jumps to its time, and the old one fades out (PreFade); a
     *  turn that goes on takes its newest time once this fade is over */
    __attribute__((noinline)) void StartPreFade()
    {
        pre_from_ = pre_.value;
        pre_.value = pre_.target;
        pre_fade_ = kPreFadeFrames;
    }
    /** A channel's crossfade from the old time's tap to w, the new one's, linearly */
    __attribute__((noinline)) float PreFade(const float* buf, float w) const
    {
        const float old = static_cast<float>(pre_fade_) / static_cast<float>(kPreFadeFrames);
        return w + old * (PreTap(buf, pre_from_) - w);
    }

    float sample_rate_ = 48000.f;
    daisysp::Reverb* reverb_;
    TailWatch tail_;
    Smoothed level_;
    Smoothed decay_;
    Smoothed tone_;
    Smoothed diffusion_;
    Smoothed freeze_;
    Smoothed pre_;    // the pre-delay, frames: the knob's (target) and the read's (value)
    Ducker duck_;
    size_t pre_pos_ = 0;
    size_t pre_filled_ = 0; // frames written since the pre-delay came up
    float pre_from_ = 0.f;  // the time PreFade fades out from
    uint32_t pre_fade_ = 0; // its frames left, 0 none
};

} // namespace chompi
