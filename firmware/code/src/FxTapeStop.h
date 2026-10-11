/** @file FxTapeStop.h
 *  @brief The tape stop: FRIZZ's own.
 */
#pragma once
#include "FxCommon.h"
#include "TempoClock.h"

namespace chompi
{

// Stop times in 16ths: 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars
static const uint8_t kTapeStop16ths[] = {1, 2, 4, 8, 16, 32};
// Spin-up times in 16ths: off (straight back), 1/16, 1/8, 1/4, 1/2, 1 bar
static const uint8_t kTapeStart16ths[] = {0, 1, 2, 4, 8, 16};
// Below this speed the level falls with it, to silence at a standstill, as on tape
static const float kTapeQuietRate = .05f;
// The jump to the live signal when the spin-up starts, 5ms, and the splice back to it once
// the tape is at speed, 15ms
static const float kTapeJumpFrames = 240.f;
static const float kTapeSpliceFrames = 720.f;

/** A tape stop. Pressing the key slows the tape to a standstill over the stop time, the pitch
 *  falling with it; it stays stopped, silent, while the key is held. Releasing it spins the
 *  tape back up from the live signal over the spin-up time, then splices back to the live
 *  signal once it's at speed: a head slower than the tape falls behind, so it can't land on
 *  the live signal by itself. Releasing while it slows spins up from where it is, and
 *  pressing while it spins up slows down again from there.
 *
 *  The curve bends both: linear, or a brake, fast at first and then dragging (the speed
 *  (1 - t)^k, k 1..3); the spin-up mirrors it, a motor's fast start and slow settle.
 *  The times follow the tempo, fixed when each starts.
 *  Params: 0 stop (kNumStops steps), 1 spin-up (kNumStarts steps), 2 curve, 3 depth: how far
 *  it slows, from a full stop (1, the default) to half speed and less at 0 (the SP-404MK2
 *  Stopper's DEPTH); page 2's own, 5 darken: the highs fall away with the speed (its FLT MOD).
 *  Page 2's Mix, Band and Level (FxOutput.h) stay on while it spins up after the key.
 *  The buffers are separate (SDRAM, chompi_main.cpp), a power of 2 frames per channel. */
class TapeStop : public FxBase
{
public:
    enum Param
    {
        STOP,
        START,
        CURVE,
        DEPTH,
        DARKEN = 5,
    };

    static const size_t kNumStops = sizeof(kTapeStop16ths);
    static const size_t kNumStarts = sizeof(kTapeStart16ths);

    /** frames: a power of 2 */
    FX_ONCE void Init(float sample_rate, float* buf_l, float* buf_r, size_t frames)
    {
        sample_rate_ = sample_rate;
        buf_[0] = buf_l;
        buf_[1] = buf_r;
        mask_ = frames - 1;
        max_lag_ = static_cast<float>(frames) - 4.f;
        pos_ = 0;
        held_[0] = held_[1] = 0.f;
        recording_ = false;
        tempo_ = kDefaultBpm;
        gate_.Init();
        state_ = State::IDLE;
        head_ = {0.f, 1.f};
        old_ = {0.f, 1.f};
        xfade_ = 1.f;
        xfade_inc_ = 0.f;
        n_ = 0;
        n_end_ = 1;
        for (size_t i = 0; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SetParam(DEPTH, 1.f); // a full stop
    }

    void SetTempo(float bpm) { tempo_ = bpm; }

    FRIZZ_HOT void Process(float* l, float* r)
    {
        gate_.Process();
        // off, on the live signal: the buffer (in SDRAM) waits, only this frame is kept, for
        // a press's first read, which reaches one frame behind it
        if (state_ == State::IDLE && !gate_.IsOn())
        {
            held_[0] = *l;
            held_[1] = *r;
            recording_ = false;
            return;
        }
        if (!recording_)
        {
            const size_t before = (pos_ - 1) & mask_;
            buf_[0][before] = held_[0];
            buf_[1][before] = held_[1];
            recording_ = true;
        }
        buf_[0][pos_] = *l;
        buf_[1][pos_] = *r;
        const size_t last = pos_;
        pos_ = (pos_ + 1) & mask_;

        if (gate_.TakePress())
            BeginStop();
        if (!gate_.IsOn() && (state_ == State::STOPPING || state_ == State::STOPPED))
            BeginStart();
        // the live signal passes untouched
        if (state_ == State::IDLE)
            return;

        switch (state_)
        {
        case State::STOPPING:
            if (++n_ >= n_end_)
            {
                head_.rate = floor_;
                state_ = State::STOPPED;
            }
            else
                head_.rate = floor_ + (1.f - floor_) * powf(1.f - T(), k_);
            break;
        case State::STARTING:
            if (++n_ >= n_end_)
                Splice();
            else
                head_.rate = floor_ + (1.f - floor_) * (1.f - powf(1.f - T(), k_));
            break;
        case State::SPLICING:
            if (xfade_ >= 1.f)
            {
                state_ = State::IDLE;
                return;
            }
            break;
        default:
            break;
        }
        head_.Move(max_lag_);
        // held long enough off its speed (at a depth below 1), the head nears the buffer's
        // far end: it jumps back to the live signal at the same speed, crossfaded, and falls
        // behind from there again, rather than sticking at the end, where it played the
        // buffer's oldest sound at full speed. Early enough that the head it fades from
        // doesn't reach the end either
        if (head_.lag >= max_lag_ - kTapeJumpFrames && xfade_ >= 1.f)
            Jump({0.f, head_.rate}, kTapeJumpFrames);

        float* const io[2] = {l, r};
        const bool fading = xfade_ < 1.f;
        if (fading)
        {
            old_.Move(max_lag_);
            xfade_ += xfade_inc_;
            if (xfade_ > 1.f)
                xfade_ = 1.f;
        }
        for (size_t c = 0; c < 2; c++)
        {
            const float out = Read(c, last, head_);
            if (fading)
            {
                const float from = Read(c, last, old_);
                *io[c] = from + xfade_ * (out - from);
            }
            else
                *io[c] = out;
            // darken: a lowpass that closes as the tape slows, from open at speed
            if (darken_ > 0.f)
            {
                const float r2 = head_.rate * head_.rate;
                const float coeff = 1.f - darken_ * (1.f - r2 * r2);
                lp_[c] += fmaxf(coeff, .02f) * (*io[c] - lp_[c]);
                *io[c] = lp_[c];
            }
            else
                lp_[c] = *io[c];
        }
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case STOP:
            stop_idx_ = StepIndex(val, kNumStops);
            break;
        case START:
            start_idx_ = StepIndex(val, kNumStarts);
            break;
        case CURVE:
            curve_ = 1.f + 2.f * val;
            break;
        case DEPTH:
            // the speed the slowing stops at: 0, a full stop, at the top; half speed at 0
            floor_ = .5f * (1.f - val);
            break;
        case DARKEN:
            darken_ = val;
            break;
        default:
            break;
        }
    }

    /** Doing no more than it does off: its key's fade done and the tape back on the live
     *  signal, which a spin-up after a release reaches only up to a bar later (the bench) */
    inline bool Resting() const { return Idle() && state_ == State::IDLE; }
    /** For page 2's Mix and Level (FxOutput.h): on while the tape is off its speed, also
     *  after the key, at full while it does */
    inline bool Quiet() const { return Resting(); }
    inline float Fade() const { return state_ == State::IDLE ? FxBase::Fade() : 1.f; }

private:
    enum class State
    {
        IDLE,     // the live signal
        STOPPING, // slowing down
        STOPPED,  // silent, the key held
        STARTING, // spinning up
        SPLICING, // at speed, crossfading back to the live signal
    };

    /** A read head: how far behind the live signal, and its speed (1: the tape's) */
    struct Head
    {
        float lag, rate;
        /** Once per sample: a head slower than the tape falls behind */
        void Move(float max_lag)
        {
            lag += 1.f - rate;
            if (lag > max_lag)
                lag = max_lag;
        }
    };

    inline float Read(size_t c, size_t last, const Head& h) const
    {
        const float level = h.rate < kTapeQuietRate ? h.rate / kTapeQuietRate : 1.f;
        return level * ReadFrac(buf_[c], mask_, last, h.lag);
    }

    /** Frames of n 16ths at the tempo */
    uint32_t Frames16ths(uint8_t n) const
    {
        return static_cast<uint32_t>(static_cast<float>(n) * 15.f * sample_rate_ / tempo_);
    }

    /** Sets the stop's or spin-up's length, starting at t (0..1) along it */
    void Begin(uint32_t frames, float t)
    {
        n_end_ = frames;
        n_ = static_cast<uint32_t>(t * static_cast<float>(frames));
    }
    /** 0..1 along the stop or spin-up. Counted in frames: adding up a float per sample
     *  drifts by percents over seconds */
    inline float T() const { return static_cast<float>(n_) / static_cast<float>(n_end_); }

    /** How far a speed is from the depth's floor to full speed, 0..1 */
    inline float Above(float rate) const
    {
        return fclamp((rate - floor_) / (1.f - floor_), 0.f, 1.f);
    }

    /** The key went down: slow from the speed the tape is at now */
    void BeginStop()
    {
        if (state_ == State::IDLE)
            head_ = {0.f, 1.f};
        k_ = curve_;
        // where along the curve the tape is at this speed
        Begin(Frames16ths(kTapeStop16ths[stop_idx_]), 1.f - powf(Above(head_.rate), 1.f / k_));
        state_ = State::STOPPING;
    }

    /** The key went up: jump to the live signal at the speed the tape is at, and spin up from
     *  there, or straight back to the live signal with the spin-up off */
    void BeginStart()
    {
        const uint8_t n = kTapeStart16ths[start_idx_];
        if (n == 0)
        {
            Splice();
            return;
        }
        Jump({0.f, head_.rate}, kTapeJumpFrames);
        k_ = curve_;
        Begin(Frames16ths(n), 1.f - powf(1.f - Above(head_.rate), 1.f / k_));
        state_ = State::STARTING;
    }

    /** At speed: crossfade to the live signal */
    void Splice()
    {
        Jump({0.f, 1.f}, kTapeSpliceFrames);
        state_ = State::SPLICING;
    }

    /** The head moves to h, crossfaded from where it was over frames */
    void Jump(Head h, float frames)
    {
        old_ = head_;
        head_ = h;
        xfade_ = 0.f;
        xfade_inc_ = 1.f / frames;
    }

    float sample_rate_;
    float* buf_[2];
    size_t mask_;
    float max_lag_;
    size_t pos_; // the next frame to write
    float held_[2];  // the last frame while it didn't record
    bool recording_; // whether it did last sample
    float tempo_;
    State state_;
    Head head_;
    Head old_;         // the head a jump crossfades from
    float xfade_;      // 0..1 from old_ to head_
    float xfade_inc_;
    uint32_t n_ = 0;     // frames along the stop or the spin-up ...
    uint32_t n_end_ = 1; // ... of this many
    float k_ = 1.f;    // the curve, fixed when a stop or spin-up starts
    size_t stop_idx_ = 0, start_idx_ = 0;
    float curve_ = 1.f;
    float floor_ = 0.f;   // the speed a stop slows to (depth)
    float darken_ = 0.f;
    float lp_[2] = {0.f, 0.f}; // darken's lowpass
};

} // namespace chompi
