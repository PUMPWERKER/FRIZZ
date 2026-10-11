/** @file FxFreezer.h
 *  @brief The freezer, a beat repeat.
 *
 *  Ported from Bastl Instruments' Kastle 2 FX Wizard (github.com/bastl-instruments/kastle2,
 *  code/src/apps/FxWizard/), rewritten from Kastle's 44kHz fixed point to 48kHz float.
 *  Copyright (c) 2024 Marek Mach, Vaclav Mach (Bastl Instruments), MIT License: see
 *  LICENSE-kastle2.
 */
#pragma once
#include "FxCommon.h"
#include "TempoClock.h"

namespace chompi
{

// Freezer lengths, a bar divided by, short to long: 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2T, 1/2, 1 bar
static const uint8_t kFreezerBarDivisions[] = {16, 12, 8, 6, 4, 3, 2, 1};
// Roll stages, off then long to short: each halving of the loop comes after this many repeats
// at the starting length, so every stage of the roll lasts as long as the first
static const uint8_t kFreezerRollStages[] = {0, 8, 4, 2, 1};

/** Kastle's freezer as a beat repeat. Pressing the key arms it; on the next 16th it
 *  starts recording, passing the live signal through for one loop length, then loops what
 *  it recorded until the key is released. It keeps recording past the loop for as long as
 *  the buffer lasts, so the length can be turned up while it repeats. The roll halves the
 *  loop as it repeats, down to 1/64 bar, a beat repeat's build-up; it isn't Kastle's.
 *  Pressed again while a release still fades the loop out, the loop plays on until the 16th
 *  that starts the new capture, and hands over to it in a short crossfade instead of
 *  dropping to the live signal at once.
 *  Params: 0 length (kNumLengths steps), 1 feedback, 2 roll (kNumRolls steps), 3 stereo;
 *  page 2's own, 5 gate: how much of each repeat is heard, all of it (0) down to its first
 *  eighth, a stutter (the rest silent, faded in and out over 2ms).
 *  The freezer's buffers are separate (SDRAM, chompi_main.cpp), kFreezerFrames per channel. */
class Freezer : public FxBase
{
public:
    enum Param
    {
        LENGTH,
        FEEDBACK,
        ROLL,
        STEREO,
        GATE = 5,
    };

    static const size_t kNumLengths = sizeof(kFreezerBarDivisions);
    static const size_t kNumRolls = sizeof(kFreezerRollStages);

    FX_ONCE void Init(float sample_rate, float* buf_l, float* buf_r, size_t frames)
    {
        sample_rate_ = sample_rate;
        buf_[0] = buf_l;
        buf_[1] = buf_r;
        frames_ = frames;
        state_ = State::IDLE;
        start_ = false;
        rearmed_ = false;
        handover_ = 0;
        gate_.Init();
        tempo_ = 120.f;
        Restart();

        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        ApplyParams();
    }

    /** Once per block: the tempo, plus one call per clock pulse in this block, with the
     *  clock's position (TempoClock::Pulse) */
    void SetTempo(float bpm)
    {
        if (bpm != tempo_)
        {
            tempo_ = bpm;
            UpdateLengths();
        }
    }
    void ClockPulse(uint32_t pos)
    {
        if (pos % kPulsesPer16th == 0 && (state_ == State::ARMED || rearmed_))
            start_ = true;
    }

    FRIZZ_HOT void Process(float* l, float* r)
    {
        ApplyParams();
        const float gate = gate_.Process();

        // back to idle once the release has faded out
        if (state_ != State::IDLE && gate_.Asleep())
        {
            state_ = State::IDLE;
            rearmed_ = false;
        }

        if (start_)
        {
            start_ = false;
            if (state_ == State::ARMED)
            {
                state_ = State::RUNNING;
                Restart();
            }
            else if (state_ == State::RUNNING && rearmed_)
            {
                rearmed_ = false;
                Handover();
                Restart();
            }
        }

        // idle or waiting for the 16th: the live signal passes
        if (state_ != State::RUNNING)
            return;

        float* const io[2] = {l, r};

        // record forward until the buffer is full
        const bool recording = written_ < frames_;
        if (recording)
        {
            buf_[0][written_] = *l;
            buf_[1][written_] = *r;
            written_++;
        }

        for (size_t c = 0; c < 2; c++)
        {
            // the first pass: still recording the loop, the live signal passes
            float wet = *io[c];
            if (written_ > len_[c] || !recording)
                wet = Repeat(c, *io[c]);

            // a new capture taking over from a loop still playing: crossfaded
            if (handover_ && old_len_[c])
            {
                size_t& pos = old_pos_[c];
                if (pos >= old_len_[c])
                {
                    old_seam_[c] = pos;
                    pos = 0;
                }
                const float old = LoopSample(c, pos++, old_len_[c], old_seam_[c], old_written_);
                const float t = 1.f - static_cast<float>(handover_) / static_cast<float>(kXfadeFrames);
                wet = old + t * (wet - old);
            }

            *io[c] += gate * (wet - *io[c]);
        }
        if (handover_)
            handover_--;
    }

    void SetOn(bool on) override
    {
        // from FxChain::Block, in the audio callback between two blocks, as everything here
        if (gate_.SetOn(on))
        {
            // a new capture, also during a release fade-out; while the old loop still plays,
            // it goes on until the new one starts
            if (state_ == State::RUNNING)
                rearmed_ = true;
            else
                state_ = State::ARMED;
        }
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case LENGTH:
            length_idx_ = StepIndex(val, kNumLengths);
            break;
        case FEEDBACK:
            // Kastle's maps: input in 0 / .3 / .8 and the loop kept 1 / 1 / .9 at 0 / .75 / 1
            if (val < .75f)
            {
                fb_in_ = val / .75f * .3f;
                fb_keep_ = 1.f;
            }
            else
            {
                const float t = (val - .75f) / .25f;
                fb_in_ = .3f + t * .5f;
                fb_keep_ = 1.f - t * .1f;
            }
            break;
        case STEREO:
            // Kastle: the left loop up to 2000 samples at 44kHz longer
            stereo_ = static_cast<size_t>(val * kMaxStereoFrames);
            break;
        case GATE:
            gate_frac_ = 1.f - .875f * val;
            break;
        case ROLL:
            roll_stage_ = kFreezerRollStages[StepIndex(val, kNumRolls)];
            // turned off: back to the full length, and a roll turned on again starts over
            if (roll_stage_ == 0)
                roll_reset_ = true;
            break;
        default:
            break;
        }
        // the lengths and the roll belong to the audio callback, which picks this up
        params_changed_ = true;
    }

private:
    enum class State
    {
        IDLE,
        ARMED,
        RUNNING,
    };

    static const size_t kRollShortest = 64; // the roll stops halving at 1/64 bar
    static const size_t kMaxStereoFrames = 2180; // 45ms
    static const size_t kXfadeFrames = 240;      // 5ms, like the looper's
    static constexpr float kGateRampFrames = 96.f; // 2ms

    /** In the audio callback: what SetParam changed */
    void ApplyParams()
    {
        if (!params_changed_)
            return;
        params_changed_ = false;
        if (roll_reset_)
        {
            roll_reset_ = false;
            repeats_ = halvings_ = 0;
        }
        UpdateLengths();
    }

    /** Channel c's loop, past its first pass: its next sample, with the seam crossfade, and
     *  in the input overdubbed for the feedback */
    float Repeat(size_t c, float in)
    {
        // can't loop more than has been recorded
        const size_t target = len_[c];
        size_t len = target < written_ ? target : written_;
        if (pos_[c] >= len)
        {
            // the seam: where the loop just ended, which the crossfade below follows on
            seam_[c] = pos_[c];
            pos_[c] = 0;
            // the right loop has no stereo offset, so it counts the repeats for the roll
            if (c == 1 && Repeated())
                len = len_[c] < written_ ? len_[c] : written_;
        }
        const size_t pos = pos_[c];
        float wet = LoopSample(c, pos, len, seam_[c], written_);
        // the gate: only the start of each repeat heard, with short ramps at both ends
        if (gate_frac_ < 1.f)
        {
            const float open = gate_frac_ * static_cast<float>(len);
            const float p = static_cast<float>(pos);
            const float g = fminf(fminf(p / kGateRampFrames, 1.f), (open - p) / kGateRampFrames);
            wet *= fclamp(g, 0.f, 1.f);
        }

        // feedback: the input overdubbed into the loop, which fades a little
        float* const b = buf_[c];
        if (fb_in_ > .0001f)
            b[pos] = 2.f * SoftClip(.5f * (b[pos] * fb_keep_ + in * fb_in_));

        pos_[c] = pos + 1;
        return wet;
    }

    /** A loop's sample at pos. Kastle has no seam crossfade; this blends the loop start with
     *  what was recorded just after the seam (on the first repeat, the loop's end), so each
     *  repeat follows on from the one before seamlessly, also when the roll shortens it */
    float LoopSample(size_t c, size_t pos, size_t len, size_t seam, size_t written) const
    {
        const float* const b = buf_[c];
        float wet = b[pos];
        if (!seam)
            seam = len;
        const size_t xfade = len / 4 < kXfadeFrames ? len / 4 : kXfadeFrames;
        if (pos < xfade && seam + pos < written)
        {
            const float t = static_cast<float>(pos) / static_cast<float>(xfade);
            wet = wet * t + b[seam + pos] * (1.f - t);
        }
        return wet;
    }

    /** A new capture starting while the old loop plays: the old one goes on for the
     *  crossfade, read as it was. The new capture records over its start, which by then the
     *  seam crossfade reads less and less of. A channel still in its first pass was passing
     *  the live signal, as the new capture's first pass does: nothing to fade from */
    void Handover()
    {
        for (size_t c = 0; c < 2; c++)
        {
            const bool looping = written_ > len_[c] || written_ >= frames_;
            old_len_[c] = looping ? (len_[c] < written_ ? len_[c] : written_) : 0;
            old_pos_[c] = pos_[c];
            old_seam_[c] = seam_[c];
        }
        old_written_ = written_;
        handover_ = kXfadeFrames;
    }

    void Restart()
    {
        written_ = 0;
        pos_[0] = pos_[1] = 0;
        seam_[0] = seam_[1] = 0;
        repeats_ = 0;
        halvings_ = 0;
        UpdateLengths();
    }

    /** At the end of each repeat: moves the roll on. Returns whether the loop got shorter */
    bool Repeated()
    {
        if (roll_stage_ == 0)
            return false;
        // each stage lasts roll_stage_ repeats at the starting length
        if (++repeats_ < (static_cast<size_t>(roll_stage_) << halvings_))
            return false;
        repeats_ = 0;
        if ((base_len_ >> (halvings_ + 1)) < bar_ / kRollShortest)
            return false;
        halvings_++;
        UpdateLengths();
        return true;
    }

    void UpdateLengths()
    {
        bar_ = static_cast<size_t>(240.f * sample_rate_ / tempo_);
        base_len_ = bar_ / kFreezerBarDivisions[length_idx_];
        const size_t len = base_len_ >> halvings_;

        const size_t max = frames_ - 1;
        len_[0] = len + stereo_ < max ? len + stereo_ : max;
        len_[1] = len < max ? len : max;
    }

    float sample_rate_;
    float* buf_[2];
    size_t frames_;
    State state_;
    bool start_; // ClockPulse and Process both run in the audio callback
    bool rearmed_;            // pressed again while running: a new capture at the next 16th
    size_t handover_;         // samples of the crossfade from the old loop left
    size_t old_len_[2] = {0, 0}; // the old loop, per channel (0: nothing to fade from)
    size_t old_pos_[2] = {0, 0};
    size_t old_seam_[2] = {0, 0};
    size_t old_written_ = 0;
    float tempo_;
    size_t written_;  // frames recorded since the capture started
    size_t pos_[2];   // loop read position, per channel
    size_t seam_[2];  // where the last repeat ended, per channel (0: none yet)
    size_t len_[2];   // target loop length, per channel
    size_t bar_ = 0;      // a bar at the tempo, in frames
    size_t base_len_ = 0; // the length knob's loop length, before the roll
    size_t repeats_ = 0;  // repeats in the roll's current stage
    size_t halvings_ = 0; // how often the roll has halved the loop
    size_t length_idx_ = 0;
    size_t stereo_ = 0;
    uint8_t roll_stage_ = 0;
    volatile bool params_changed_ = false; // set by SetParam, for ApplyParams
    volatile bool roll_reset_ = false;     // the roll was turned off
    float fb_in_ = 0.f, fb_keep_ = 1.f;
    float gate_frac_ = 1.f; // the gate: the share of each repeat heard
};

} // namespace chompi
