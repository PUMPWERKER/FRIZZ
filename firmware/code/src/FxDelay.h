/** @file FxDelay.h
 *  @brief TEMPO's tempo-synced delay as a send.
 */
#pragma once
#include "FxCommon.h"
#include "granularDelay.h"
#include "TempoClock.h"

namespace chompi
{

/** TEMPO's tempo-synced delay (granularDelay.h) as a send.
 *  Params: 0 division (9 steps), 1 feedback, 2 random (bipolar, 0.5 = off), 3 level; page 2:
 *  4 freeze (the input shut and the loop's gain up to exactly 1, so the buffer loops; TEMPO's
 *  buffer lock, by degrees; no random events while frozen), 5 damping (bipolar: left a low cut, right a high cut in the
 *  feedback, the SP-404MK2's L DAMP and H DAMP; the centre off), 7 ducking (the echoes duck
 *  under what goes in). Page 2's 6, Band, filters what goes in (FxChain.h). */
class DelaySend : public FxBase
{
public:
    enum Param
    {
        DIVISION,
        FEEDBACK,
        RANDOM,
        LEVEL,
        FREEZE,
        DAMPING,
        DUCKING = 7,
    };

    static const size_t kNumDivisions = kNumDelayDivs;

    FX_ONCE void Init(float sample_rate, float* buffer, size_t buffer_frames)
    {
        sample_rate_ = sample_rate;
        delay_.Init(buffer, buffer_frames);
        sleep_samples_ = static_cast<uint32_t>(buffer_frames);
        gate_.Init();
        level_.Reset(0.f);
        feedback_.Reset(.3f); // granularDelay's own
        freeze_.Reset(0.f);
        duck_.Init();
    }

    /** Once per block: the tempo, plus one call per clock pulse in this block, with the
     *  clock's position (TempoClock::Pulse). Every 8th note is an edge, where the delay rolls
     *  its random events, only while its key is on: the tail of one that's off is plain
     *  echoes. */
    void SetTempo(float bpm) { delay_.SetTempo(bpm); }
    void ClockPulse(uint32_t pos)
    {
        // no events while frozen: a hard-panned one would push the held loop up
        if (pos % kPulsesPerEdge == 0)
            delay_.setClockEdge(gate_.IsOn() && freeze_.target < .5f);
    }

    /** Feeds in_l / in_r into the delay (while on) and adds its return to *out_l / *out_r */
    void Process(float in_l, float in_r, float* out_l, float* out_r)
    {
        const float gate = gate_.Process();
        const bool silent_in = gate_.Asleep();
        if (tail_.Sleeping(silent_in, sleep_samples_))
            return;
        const float level = level_.Process();
        // the feedback slews like the other knobs, so turning it doesn't zipper the repeats;
        // the freeze takes the loop's gain up to exactly 1 as it shuts the input, making up
        // for the pan every pass goes through
        const bool fb_moved = feedback_.Settle();
        if (freeze_.Settle() || fb_moved)
            delay_.setFeedbackAmount(feedback_.value * .975f * (1.f - freeze_.value)
                                     + freeze_.value / granularDelay::CentreGain());
        const float in_gain = gate * (1.f - freeze_.value);

        float dl = 0.f, dr = 0.f;
        delay_.write(in_l * in_gain, in_r * in_gain);
        delay_.read(&dl, &dr);
        tail_.Track(silent_in, dl, dr);
        // ducking, under what goes in (as heard: the key's fade on it)
        if (duck_.amount > 0.f)
        {
            const float g = duck_.Gain(in_l * gate, in_r * gate);
            dl *= g;
            dr *= g;
        }

        *out_l += dl * level;
        *out_r += dr * level;
    }

    /** Off and its echoes rung out for the whole buffer, so none can come back: nothing to
     *  add, and its meter isn't needed */
    inline bool Sleeping() const { return tail_.quiet >= sleep_samples_; }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case DIVISION:
            delay_.setDivision(StepIndex(val, kNumDivisions));
            break;
        case FEEDBACK:
            feedback_.target = val;
            break;
        case RANDOM:
            delay_.setRandom(val);
            break;
        case LEVEL:
            level_.target = val;
            break;
        case FREEZE:
            freeze_.target = val;
            break;
        case DAMPING:
        {
            // the centre off; left a highpass up to 1kHz, right a lowpass down to 1kHz
            float hi = 1.f, lo = 0.f;
            if (val > .5f)
                hi = OnePoleCoeff(20000.f * powf(.05f, 2.f * (val - .5f)), sample_rate_);
            else if (val < .5f)
                lo = OnePoleCoeff(20.f * powf(50.f, 2.f * (.5f - val)), sample_rate_);
            delay_.setDamping(hi, lo);
            break;
        }
        case DUCKING:
            duck_.amount = val;
            break;
        default:
            break;
        }
    }

private:
    float sample_rate_ = 48000.f;
    granularDelay delay_;
    Smoothed level_;
    Smoothed feedback_;
    Smoothed freeze_;
    Ducker duck_;
    TailWatch tail_;
    uint32_t sleep_samples_ = 0; // the buffer's length: silent that long, it holds nothing
};

} // namespace chompi
