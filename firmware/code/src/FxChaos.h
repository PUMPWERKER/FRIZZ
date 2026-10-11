/** @file FxChaos.h
 *  @brief The chaos key (#65), on the 11th white key: an FX slot like any other (held, latched,
 *  part of scenes) that makes no sound of its own but plays with what's there, on a grid of the
 *  FX clock (TempoClock.h), in two ways:
 *   - FX gates, after the Red Panda Tensor's RAND: the effects latched (its pool, sent by
 *     FxControls) drop out of a step and come back on the next, at random. FxChain gates them.
 *   - Loop scramble, after the Chase Bliss Blooper's Scrambler: on a random step the loop plays
 *     another step of itself, and on the next step without one it plays in place again; the
 *     loop's own position runs on, so the FX clock and the loop's end don't move. The engine
 *     hands it to the looper (Looper::Scramble).
 *  Chance decides only when the effects act, never how they sound.
 *
 *  Params: 0 FX chance (each pooled effect drops out of a step with up to 1 in 2), 1 scramble
 *  chance (up to every step elsewhere), 2 grid (1/16, 1/8, 1/4, 1/2, a bar, stepped), 3 random to
 *  pattern: a bar's steps are kept once rolled, and each rolls again with 1 minus it, so at 0
 *  every step is new and at 1 one bar repeats and grooves. Each press (the key coming on) rolls a
 *  fresh bar. No page 2.
 */
#pragma once
#include "FxCommon.h"
#include "TempoClock.h"

namespace chompi
{

class Chaos : public FxBase
{
public:
    enum Param
    {
        FX_CHANCE,
        SCRAMBLE,
        GRID,
        PATTERN,
    };

    static const size_t kNumGrids = 5;
    // a bar's steps at the finest grid
    static const size_t kMaxSteps = kPulsesPerBar / kPulsesPer16th;
    // FX chance at the top: each pooled effect sits out half the steps
    static constexpr float kMaxDrop = .5f;

    FX_ONCE void Init()
    {
        gate_.Init();
        rng_.Seed(0x6A09E667u);
        fx_chance_ = scramble_ = repeat_ = 0.f;
        grid_ = 1;
        rolled_ = 0;
        drops_ = 0;
        jump_ = 0.f;
        step_ = false;
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case FX_CHANCE:
            fx_chance_ = val;
            break;
        case SCRAMBLE:
            scramble_ = val;
            break;
        case GRID:
            grid_ = StepIndex(val, kNumGrids);
            break;
        case PATTERN:
            repeat_ = val;
            break;
        default:
            break;
        }
    }

    /** One call per clock pulse, with the clock's position (TempoClock::Pulse): a step on the
     *  grid takes the bar's step there, rolled anew or as it was */
    void ClockPulse(uint32_t pos)
    {
        if (gate_.TakePress())
            rolled_ = 0;
        if (!gate_.IsOn())
            return;
        const uint32_t every = GridPulses();
        if (pos % every)
            return;
        const size_t slot = (pos % kPulsesPerBar) / every;
        const uint16_t bit = static_cast<uint16_t>(1u << slot);
        if (!(rolled_ & bit) || rng_.Uniform() >= repeat_)
        {
            Roll(slot);
            rolled_ |= bit;
        }
        drops_ = drops_at_[slot];
        jump_ = jump_at_[slot];
        step_ = true;
    }

    /** Once per block, after the pulses. Off, nothing is dropped and the loop plays in place.
     *  True once per step (and once when it goes off from elsewhere), with where the loop plays
     *  it: 0 in place, otherwise 0..1 of the way round the loop's other steps */
    bool TakeStep(float* jump)
    {
        gate_.Process(); // a block's worth of the key's fade is enough: it has no sound
        if (!gate_.IsOn())
        {
            drops_ = 0;
            if (jump_ != 0.f)
            {
                jump_ = 0.f;
                step_ = true;
            }
        }
        if (!step_)
            return false;
        step_ = false;
        *jump = jump_;
        return true;
    }

    /** Bit fx: dropped from this step, if it's in the pool */
    inline uint16_t Drops() const { return drops_; }
    /** The grid in clock pulses: 1/16, 1/8, 1/4, 1/2, a bar */
    inline uint32_t GridPulses() const
    {
        static const uint32_t kGrid[kNumGrids] = {kPulsesPer16th, kPulsesPer16th * 2,
                                                  kPulsesPerBeat, kPulsesPerBeat * 2,
                                                  kPulsesPerBar};
        return kGrid[grid_];
    }

private:
    void Roll(size_t slot)
    {
        uint16_t drops = 0;
        const float p = fx_chance_ * kMaxDrop;
        for (size_t fx = 0; fx < 16; fx++)
            if (rng_.Uniform() < p)
                drops |= static_cast<uint16_t>(1u << fx);
        drops_at_[slot] = drops;
        // somewhere else round the loop, never 0
        jump_at_[slot] = rng_.Uniform() < scramble_ ? fmaxf(rng_.Uniform(), 1e-6f) : 0.f;
    }

    Rng rng_;
    float fx_chance_, scramble_, repeat_;
    size_t grid_;
    uint16_t rolled_;                // bit slot: rolled since the last press
    uint16_t drops_at_[kMaxSteps];   // a bar's steps: what each drops,
    float jump_at_[kMaxSteps];       // and where it plays the loop
    uint16_t drops_;                 // this step's
    float jump_;
    bool step_;                      // a step since TakeStep
};

} // namespace chompi
