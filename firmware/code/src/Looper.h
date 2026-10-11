/** @file Looper.h
 *  @brief The looper core: one stereo loop in SDRAM, recorded from the dry input and played
 *  back into the wet side of the mix. See LOOPER.md for the spec.
 *
 *  Everything here runs in the audio callback. The UI (MainLoop) only posts commands
 *  (StartRecording, StopRecording, TogglePlay, Erase, EraseAtEnd, CancelErase), which are
 *  picked up at the start of
 *  the next audio block, so recording starts and ends aligned to the audio block. Lengths are
 *  counted in recorded frames, and MIDI clock tick times are on the same sample clock.
 *
 *  Commands go through a single slot that the audio callback empties once per block, so:
 *   - post at most one command per UI event; a second one in the same block replaces the first
 *   - GetState() only reflects a command from the next audio block on
 *  Speed steps (StepSemitone, StepSpeed, ResetSpeed) are counted up separately and all applied at the next
 *  block, so GetSpeed() also follows a block later.
 *
 *  Loop point: when a recording closes at length L, the input keeps being written for
 *  kXfadeFrames more frames (the post-roll). Reading position p < kXfadeFrames crossfades
 *  from the post-roll (the natural continuation of the loop's tail) into the loop's head, so
 *  the jump from L-1 back to 0 doesn't click, in either direction.
 *
 *  Speed: the read head is a frame index plus a fraction, advanced by the speed each
 *  sample and read with 4-point Hermite interpolation. Speed moves in semitones (StepSemitone)
 *  or along TAPE's ladder of 5ths and octaves (StepSpeed), glides to each new step like TAPE's
 *  default tape slew, and runs in reverse when negative. While paused, the transport knob scrubs instead (Scrub).
 *
 *  Scramble (the chaos key, FxChaos.h): the loop is heard from a number of frames on from the
 *  read head, which itself runs on as ever, so the loop's position, the FX clock and its end
 *  don't move. A new offset crossfades from the old over kXfadeFrames.
 */
#pragma once
#include "FrizzHot.h"
#include <atomic>
#include "daisy.h"
#include "daisysp.h"
#include "MidiClock.h"

using namespace daisy;

namespace chompi
{

// TAPE's loop buffer size: 2:45 of 48kHz stereo int16. The total byte size (31694848) divides
// evenly into 8K, which keeps it aligned to SDRAM pages.
static const size_t kLoopMemSize = 31694848 / 2;      // int16 samples
static const size_t kLoopMaxFrames = kLoopMemSize / 2; // stereo frames

static const size_t kXfadeFrames = 240; // 5ms loop-point crossfade at 48kHz
static const float kPlayFadeCoeff = .002f; // ~10ms fade on play / pause / erase

// Speed ladder, in semitones: octaves (12k) and fifths above them (12k + 7), as TAPE.
// Top is 2x (+12), bottom is 1/16x (-48); a ladder step below the bottom flips direction, a
// semitone step stops there.
static const int kSpeedMaxSemis = 12;
static const int kSpeedMinSemis = -48;
static const float kSpeedSlewCoeff = .0001f; // TAPE's tape-slew glide, ~0.2s

// Scrubbing while paused, as TAPE: the turns counted over each 1/8s set the scrub speed
static const size_t kScrubPeriod = 6000;
static const float kScrubPerTurn = .2f;

class Looper
{
public:
    enum class State
    {
        EMPTY,
        RECORDING,
        PLAYING,
        PAUSED,
    };

    Looper() {}
    ~Looper() {}

    void Init(int16_t* mem, MidiClock* midi_clock)
    {
        mem_ = mem;
        midi_clock_ = midi_clock;
        Clear();
    }

    // ===== commands, called from the UI =====

    /** Starts recording if the looper is empty. Quantized needs a running MIDI clock, so check
     *  CanRecordQuantized() first; a quantized start without a clock is ignored. */
    void StartRecording(bool quantized)
    {
        command_.store(quantized ? Command::RECORD_QUANTIZED : Command::RECORD_FREE);
    }
    /** Ends the recording: right away when unquantized, at the end of the bar when quantized */
    void StopRecording() { command_.store(Command::STOP_RECORDING); }
    void TogglePlay() { command_.store(Command::TOGGLE_PLAY); }
    void Erase() { command_.store(Command::ERASE); }
    /** Erases when the read head next crosses the loop point, either way; paused, at once */
    void EraseAtEnd() { command_.store(Command::ERASE_AT_END); }
    /** Takes back an EraseAtEnd that hasn't happened yet */
    void CancelErase() { command_.store(Command::CANCEL_ERASE); }

    /** One step along the 5ths-and-octaves ladder, to the next rung in the turn's direction
     *  (from between two rungs, the nearer one that way). dir > 0 turns right (faster forward /
     *  slower reverse), dir < 0 turns left. Past 1/16x the direction flips at the same speed.
     *  Taken up at the next audio block, like the commands */
    void StepSpeed(int dir) { speed_steps_.fetch_add(dir > 0 ? 1 : (dir < 0 ? -1 : 0)); }

    /** One semitone, turning as StepSpeed; stops at 2x and 1/16x, never flips */
    void StepSemitone(int dir) { semi_steps_.fetch_add(dir > 0 ? 1 : (dir < 0 ? -1 : 0)); }

    /** Back to 1x forward, at the next audio block; steps posted before it are dropped */
    void ResetSpeed()
    {
        speed_steps_.store(0);
        semi_steps_.store(0);
        speed_reset_.store(true);
    }

    /** Transport turns while paused, in encoder detents */
    void Scrub(int turns) { scrub_turns_.fetch_add(turns); }

    /** Heard offset frames on from the read head, 0 in place (see the file comment). From the
     *  audio callback, between blocks */
    void Scramble(size_t offset)
    {
        if (offset >= length_)
            offset = 0;
        if (offset == offset_)
            return;
        from_offset_ = offset_;
        offset_ = offset;
        jump_left_ = kXfadeFrames;
    }

    // ===== state, readable from the UI =====

    inline State GetState() const { return state_; }
    /** True once a quantized recording has been told to stop and is finishing its bar */
    inline bool IsClosing() const { return closing_; }
    /** True while an EraseAtEnd waits for the loop point */
    inline bool IsErasePending() const { return erase_at_end_; }
    /** True from the moment an erase starts fading out until the looper is empty */
    inline bool IsErasing() const { return erasing_; }
    inline bool CanRecordQuantized() const
    {
        return midi_clock_->HasClock() && midi_clock_->GetTickPeriod() > 0.f;
    }
    /** Current scrub speed while paused, 0 when not scrubbing (for the LEDs) */
    inline float GetScrub() const { return scrub_; }

    /** Current speed target: negative is reverse, 1 is the recorded speed */
    inline float GetSpeed() const { return speed_target_; }

    /** How far on from the read head the loop is heard (Scramble), in frames */
    inline size_t GetScramble() const { return offset_; }
    /** Loop length in frames, 0 until a recording closes */

    inline size_t GetLength() const { return length_; }
    /** Beats in a quantized loop, from the clock it was recorded to; 0 for an unquantized
     *  loop, or one closed early because the clock stopped */
    inline uint32_t GetBeats() const { return beats_; }
    /** The speed the read head is moving at right now, gliding towards GetSpeed() */
    inline float GetActualSpeed() const { return speed_; }

    /** Playback position 0-1 through the loop */
    inline float GetPosition() const
    {
        return length_ > 0 ? static_cast<float>(play_pos_) / static_cast<float>(length_) : 0.f;
    }

    // ===== audio =====

    /** Records from in and plays into out, one block. */
    FRIZZ_HOT void Process(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t size)
    {
        HandleCommand();
        HandleSpeed();

        if (state_ == State::RECORDING)
            TrackRecordingClock();
        else if (state_ == State::PAUSED)
            UpdateScrub(size);

        for (size_t i = 0; i < size; i++)
        {
            switch (state_)
            {
            case State::RECORDING:
                Write(write_pos_, in_l[i], in_r[i]);
                write_pos_++;
                out_l[i] = out_r[i] = 0.f;

                if (target_length_ > 0 && write_pos_ >= target_length_)
                    CloseLoop(target_length_, target_bars_);
                else if (write_pos_ >= kLoopMaxFrames - kXfadeFrames)
                    CloseAtMaxLength();
                break;

            case State::PLAYING:
            case State::PAUSED:
                // finish the post-roll: keep writing the input past the loop end
                if (postroll_ < kXfadeFrames)
                {
                    Write(length_ + postroll_, in_l[i], in_r[i]);
                    postroll_++;
                }

                // the slews only while they move: a fonepole at its target stays there
                if (speed_ != speed_target_)
                {
                    // the glide stalls a hair short of its target, where a step rounds to
                    // nothing (~0.06% of it): from there it's at the step's speed exactly,
                    // or the loop would drift against the clock it was recorded to
                    const float was = speed_;
                    daisysp::fonepole(speed_, speed_target_, kSpeedSlewCoeff);
                    if (speed_ == was)
                        speed_ = speed_target_;
                }
                if (scrub_ != scrub_target_)
                {
                    daisysp::fonepole(scrub_, scrub_target_, kSpeedSlewCoeff);
                    if (fabsf(scrub_) < .0001f)
                        scrub_ = 0.f;
                }

                {
                    // playing: audible at the ladder speed. Paused: audible only while
                    // scrubbing, at the scrub speed. Erasing: fade out.
                    const bool playing = state_ == State::PLAYING;
                    const bool audible = !erasing_ && (playing || scrub_ != 0.f);
                    const float fade_target = audible ? 1.f : 0.f;
                    if (fade_ != fade_target)
                        daisysp::fonepole(fade_, fade_target, kPlayFadeCoeff);

                    // the fade-out after a pause or erase keeps moving at play speed, so it
                    // doesn't freeze on one sample (a DC thump)
                    if (fading_out_ && (fade_ <= .0001f || scrub_target_ != 0.f))
                        fading_out_ = false;

                    if (fade_ > .0001f)
                    {
                        if (__builtin_expect((offset_ | jump_left_) != 0, 0))
                            ReadScrambled(&out_l[i], &out_r[i]);
                        else
                            ReadInterpolated(play_pos_, &out_l[i], &out_r[i]);
                        out_l[i] *= fade_;
                        out_r[i] *= fade_;
                        // an erase waiting for the loop point starts its fade there
                        if (Advance(playing || fading_out_ ? speed_ : scrub_) && playing
                            && erase_at_end_)
                            BeginErase();
                    }
                    else
                    {
                        out_l[i] = out_r[i] = 0.f;
                    }
                }

                if (erasing_ && fade_ <= .0001f)
                    Clear();
                break;

            case State::EMPTY:
            default:
                out_l[i] = out_r[i] = 0.f;
                break;
            }
        }
    }

private:
    enum class Command : uint8_t
    {
        NONE,
        RECORD_FREE,
        RECORD_QUANTIZED,
        STOP_RECORDING,
        TOGGLE_PLAY,
        ERASE,
        ERASE_AT_END,
        CANCEL_ERASE,
    };

    /** One step of the ladder, see StepSpeed */
    void ApplyStep(int dir)
    {
        // turning right means faster when forward, slower when in reverse
        const int way = ((dir > 0) != reverse_) ? 1 : -1;
        int semis = semis_ + way;
        while (!IsRung(semis))
            semis += way;

        if (semis > kSpeedMaxSemis)
            return; // already at 2x
        if (semis < kSpeedMinSemis)
        {
            reverse_ = !reverse_; // through the slowest step: flip, same speed
            semis = semis_;
        }
        SetSemis(semis);
    }

    /** One semitone, see StepSemitone */
    void ApplySemitone(int dir)
    {
        const int semis = semis_ + (((dir > 0) != reverse_) ? 1 : -1);
        if (semis <= kSpeedMaxSemis && semis >= kSpeedMinSemis)
            SetSemis(semis);
    }

    void SetSemis(int semis)
    {
        semis_ = semis;
        speed_target_ = (reverse_ ? -1.f : 1.f) * powf(2.f, semis_ / 12.f);
    }

    /** Back to 1x forward */
    void ApplyResetSpeed()
    {
        semis_ = 0;
        reverse_ = false;
        speed_target_ = 1.f;
    }

    /** The speed steps and resets posted since the last block */
    void HandleSpeed()
    {
        if (speed_reset_.exchange(false))
            ApplyResetSpeed();
        int semis = semi_steps_.exchange(0);
        for (; semis > 0; semis--)
            ApplySemitone(1);
        for (; semis < 0; semis++)
            ApplySemitone(-1);
        int steps = speed_steps_.exchange(0);
        for (; steps > 0; steps--)
            ApplyStep(1);
        for (; steps < 0; steps++)
            ApplyStep(-1);
    }

    void HandleCommand()
    {
        const Command cmd = command_.exchange(Command::NONE);

        switch (cmd)
        {
        case Command::RECORD_FREE:
        case Command::RECORD_QUANTIZED:
        {
            const bool quantized = cmd == Command::RECORD_QUANTIZED;
            if (state_ != State::EMPTY || (quantized && !CanRecordQuantized()))
                break;

            state_ = State::RECORDING;
            quantized_ = quantized;
            closing_ = false;
            write_pos_ = 0;
            target_length_ = 0;
            target_bars_ = 0;
            start_ticks_ = midi_clock_->GetTicks();
            start_locks_ = midi_clock_->GetLocks();
            first_tick_time_ = 0;
            first_tick_count_ = 0;
            have_first_tick_ = false;
            fit_ = TickFit();
            break;
        }

        case Command::STOP_RECORDING:
            if (state_ != State::RECORDING || closing_)
                break;

            if (!quantized_ || TickPeriod() <= 0.f)
            {
                // unquantized, or a clock that just locked again and has no period yet: as
                // when the clock goes away, it closes now rather than on a bar of length 0
                CloseLoop(write_pos_);
            }
            else
            {
                // record to the end of the bar in progress. A press exactly on a bar line
                // starts the next bar (strict rule, LOOPER.md 1.3).
                const float bar = kTicksPerBar * TickPeriod();
                const uint32_t bars = static_cast<uint32_t>(write_pos_ / bar) + 1;
                target_length_ = static_cast<size_t>(bars * bar + .5f);
                target_bars_ = bars;
                closing_ = true;

                if (target_length_ > kLoopMaxFrames - kXfadeFrames)
                    CloseAtMaxLength();
                else if (write_pos_ >= target_length_)
                    CloseLoop(target_length_, target_bars_);
            }
            break;

        case Command::TOGGLE_PLAY:
            if (erasing_)
                break;
            // a scrub left over from the last pause mustn't carry into the next one
            scrub_turns_.store(0);
            scrub_ = scrub_target_ = 0.f;
            scrub_count_ = 0;
            if (state_ == State::PLAYING)
            {
                state_ = State::PAUSED;
                fading_out_ = true;
            }
            else if (state_ == State::PAUSED)
            {
                state_ = State::PLAYING;
                fading_out_ = false;
            }
            break;

        case Command::ERASE:
            if (state_ == State::RECORDING)
                Clear(); // nothing is audible yet, so no fade needed
            else if (state_ != State::EMPTY)
                BeginErase();
            break;

        case Command::ERASE_AT_END:
            if (state_ == State::PLAYING && !erasing_)
                erase_at_end_ = true;
            else if (state_ == State::PAUSED)
                BeginErase(); // nothing to wait for
            break;

        case Command::CANCEL_ERASE:
            erase_at_end_ = false;
            break;

        case Command::NONE:
        default:
            break;
        }
    }

    /** Fades out at play speed, then Clear() in Process */
    void BeginErase()
    {
        erase_at_end_ = false;
        erasing_ = true;
        fading_out_ = true;
        state_ = State::PAUSED;
    }

    /** Snapshots the first tick after the record press, and closes a quantized recording
     *  immediately if the clock goes away (LOOPER.md 1.3), also if it came back at once: its
     *  ticks then don't count from the press any more. While it records to the end of its
     *  bar, the end follows the ticks still coming in, so the length is measured over the
     *  whole loop and not only up to the press (LOOPER.md 2.2) */
    void TrackRecordingClock()
    {
        if (!quantized_)
            return;

        if (!midi_clock_->HasClock() || midi_clock_->GetLocks() != start_locks_)
        {
            CloseLoop(write_pos_);
            return;
        }

        const uint32_t ticks = midi_clock_->GetTicks();
        if (!have_first_tick_ && ticks != start_ticks_)
        {
            have_first_tick_ = true;
            first_tick_time_ = midi_clock_->GetLastTickTime();
            first_tick_count_ = ticks;
            fit_ticks_ = ticks;
            fit_.Add(0., 0.);
            last_x_ = last_y_ = 0.;
            last_late_ = false;
        }
        else if (have_first_tick_ && ticks != fit_ticks_)
        {
            // one point a block: ticks that came in one block share its time
            fit_ticks_ = ticks;
            const double x = static_cast<double>(ticks - first_tick_count_);
            const double y = static_cast<double>(midi_clock_->GetLastTickTime() - first_tick_time_);
            // the last point came far later than its ticks take, and this one far sooner after
            // it: that one was late, catching up just before this one (a host after a stall),
            // so its time is off by most of a tick and it goes. Only both together: a clock
            // that's merely early once (restarted) or late once (a lost tick) is fitted as before.
            // The first point has no time before it to tell: followed this soon, it goes too
            const float period = midi_clock_->GetTickPeriod();
            const double ticks_in = .5 * period * (x - last_x_);
            const bool soon = period > 0.f && y - last_y_ < ticks_in;
            if (soon && (last_late_ || x - last_x_ == x))
                fit_.Remove(last_x_, last_y_);
            last_late_ = period > 0.f && y - last_y_ > 3. * ticks_in; // over 1.5 ticks' time a tick
            fit_.Add(x, y);
            last_x_ = x;
            last_y_ = y;
        }

        if (closing_ && TickPeriod() > 0.f)
            target_length_ = static_cast<size_t>(target_bars_ * kTicksPerBar * TickPeriod() + .5f);
    }

    /** Tick period in samples, from the ticks since the record press once there's a beat of
     *  them, otherwise the clock's smoothed period (LOOPER.md 2.2) */
    float TickPeriod() const
    {
        const uint32_t ticks = midi_clock_->GetTicks() - first_tick_count_;
        if (have_first_tick_ && ticks >= kTicksPerBeat && fit_.Ready())
            return static_cast<float>(fit_.Slope());

        return midi_clock_->GetTickPeriod();
    }

    /** A least-squares line through the ticks' (count, time) since the record press: its slope
     *  is the tick period. A tick's time is only as precise as the audio block it came in (and
     *  over USB, the 1 ms frame it was sent in); the line averages that out over every tick,
     *  where the span from the first tick to the last keeps the error of both ends. Updated
     *  as in Welford's algorithm, which stays precise over a 2:45 recording; in doubles, once
     *  a block that a tick came in */
    struct TickFit
    {
        double n = 0., mean_x = 0., mean_y = 0., sxx = 0., sxy = 0.;
        void Add(double x, double y)
        {
            n += 1.;
            const double dx = x - mean_x;
            mean_x += dx / n;
            mean_y += (y - mean_y) / n;
            sxx += dx * (x - mean_x);
            sxy += dx * (y - mean_y);
        }
        /** Takes back a point Add() took, the last one (Add backwards) */
        void Remove(double x, double y)
        {
            if (n <= 1.)
            {
                *this = TickFit();
                return;
            }
            const double mx = mean_x, my = mean_y;
            n -= 1.;
            mean_x = (mx * (n + 1.) - x) / n;
            mean_y = (my * (n + 1.) - y) / n;
            sxx -= (x - mean_x) * (x - mx);
            sxy -= (x - mean_x) * (y - my);
        }
        inline bool Ready() const { return n >= 2. && sxx > 0.; }
        inline double Slope() const { return sxy / sxx; }
    };

    /** Hit the 2:45 limit: unquantized keeps everything, quantized cuts back to the last
     *  complete bar (LOOPER.md 1.3a) */
    void CloseAtMaxLength()
    {
        size_t length = write_pos_;
        uint32_t bars = 0;

        if (quantized_ && TickPeriod() > 0.f)
        {
            const float bar = kTicksPerBar * TickPeriod();
            bars = static_cast<uint32_t>(length / bar);
            if (bars > 0)
                length = static_cast<size_t>(bars * bar + .5f);
        }

        CloseLoop(length, bars);
    }

    /** bars: a quantized loop's whole bars, 0 when the length isn't on the clock's grid */
    void CloseLoop(size_t length, uint32_t bars = 0)
    {
        if (length == 0)
        {
            Clear();
            return;
        }

        length_ = length;
        beats_ = bars * kBeatsPerBar;
        // anything already recorded past the loop end is the start of the post-roll
        postroll_ = write_pos_ > length ? write_pos_ - length : 0;
        if (postroll_ > kXfadeFrames)
            postroll_ = kXfadeFrames;

        play_pos_ = 0;
        play_frac_ = 0.f;
        fade_ = 1.f; // the loop starts seamlessly, no fade in
        closing_ = false;
        state_ = State::PLAYING;
    }

    void Clear()
    {
        state_ = State::EMPTY;
        quantized_ = false;
        closing_ = false;
        erasing_ = false;
        erase_at_end_ = false;
        fading_out_ = false;
        length_ = 0;
        beats_ = 0;
        target_bars_ = 0;
        write_pos_ = 0;
        play_pos_ = 0;
        play_frac_ = 0.f;
        postroll_ = kXfadeFrames;
        target_length_ = 0;
        fade_ = 0.f;

        ApplyResetSpeed();
        speed_steps_.store(0);
        semi_steps_.store(0);
        speed_reset_.store(false);
        speed_ = 1.f;
        scrub_ = scrub_target_ = 0.f;
        scrub_turns_.store(0);
        scrub_count_ = 0;
        offset_ = from_offset_ = 0;
        jump_left_ = 0;
    }

    static inline int Mod12(int semis) { return ((semis % 12) + 12) % 12; }
    /** A point of the ladder: an octave or a fifth above one */
    static inline bool IsRung(int semis) { return Mod12(semis) == 0 || Mod12(semis) == 7; }

    /** Turns counted over each scrub period set the scrub speed for the next one */
    void UpdateScrub(size_t size)
    {
        scrub_count_ += size;
        if (scrub_count_ < kScrubPeriod)
            return;

        scrub_count_ = 0;
        scrub_target_ = daisysp::fclamp(scrub_turns_.exchange(0) * kScrubPerTurn, -2.f, 2.f);
    }

    /** Moves the read head by speed frames (signed), wrapping around the loop. True if it
     *  crossed the loop point */
    inline bool Advance(float speed)
    {
        bool wrapped = false;
        play_frac_ += speed;
        while (play_frac_ >= 1.f)
        {
            play_frac_ -= 1.f;
            if (++play_pos_ >= length_)
            {
                play_pos_ = 0;
                wrapped = true;
            }
        }
        while (play_frac_ < 0.f)
        {
            play_frac_ += 1.f;
            if (play_pos_ == 0)
            {
                play_pos_ = length_ - 1;
                wrapped = true;
            }
            else
                play_pos_--;
        }
        return wrapped;
    }

    /** frame + offset (-1..2) within the loop; a loop is under 2^31 frames (kLoopMaxFrames) */
    inline size_t Wrap(size_t frame, int offset) const
    {
        const int32_t f = static_cast<int32_t>(frame) + offset;
        if (f < 0)
            return static_cast<size_t>(f + static_cast<int32_t>(length_));
        if (f >= static_cast<int32_t>(length_))
            return static_cast<size_t>(f - static_cast<int32_t>(length_));
        return static_cast<size_t>(f);
    }

    /** 4-point Hermite interpolation around frame pos, at the read head's fraction */
    void ReadInterpolated(size_t pos, float* l, float* r) const
    {
        // on a whole frame (1x forward or back since the recording, no glide): Hermite at 0
        // is that frame itself, exactly, so it's read alone
        if (play_frac_ == 0.f)
        {
            Read(pos, l, r);
            return;
        }
        float xl[4], xr[4];
        for (int k = 0; k < 4; k++)
            Read(Wrap(pos, k - 1), &xl[k], &xr[k]);

        *l = Hermite(xl, play_frac_);
        *r = Hermite(xr, play_frac_);
    }

    /** The read head offset_ on, crossfading from from_offset_ while jump_left_ runs */
    __attribute__((noinline, cold)) void ReadScrambled(float* l, float* r)
    {
        ReadInterpolated(Shifted(offset_), l, r);
        if (jump_left_ == 0)
            return;
        float ol, or_;
        ReadInterpolated(Shifted(from_offset_), &ol, &or_);
        const float old = static_cast<float>(jump_left_) / kXfadeFrames;
        *l += (ol - *l) * old;
        *r += (or_ - *r) * old;
        jump_left_--;
    }
    inline size_t Shifted(size_t offset) const
    {
        const size_t pos = play_pos_ + offset;
        return pos >= length_ ? pos - length_ : pos;
    }

    static inline float Hermite(const float* x, float t)
    {
        const float c0 = x[1];
        const float c1 = .5f * (x[2] - x[0]);
        const float c2 = x[0] - 2.5f * x[1] + 2.f * x[2] - .5f * x[3];
        const float c3 = .5f * (x[3] - x[0]) + 1.5f * (x[1] - x[2]);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    inline void Write(size_t frame, float l, float r)
    {
        mem_[frame * 2] = static_cast<int16_t>(f2s16(l));
        mem_[frame * 2 + 1] = static_cast<int16_t>(f2s16(r));
    }

    /** Reads a frame, crossfading the loop head with the post-roll (see the file comment).
     *  Until the post-roll is complete, the frames past what's been written are skipped. */
    inline void Read(size_t frame, float* l, float* r) const
    {
        *l = s162f(mem_[frame * 2]);
        *r = s162f(mem_[frame * 2 + 1]);

        if (frame < kXfadeFrames && frame < postroll_ && frame < length_)
        {
            const float head = static_cast<float>(frame) / kXfadeFrames;
            const size_t tail = length_ + frame;
            *l = *l * head + s162f(mem_[tail * 2]) * (1.f - head);
            *r = *r * head + s162f(mem_[tail * 2 + 1]) * (1.f - head);
        }
    }

    int16_t* mem_;
    MidiClock* midi_clock_;

    std::atomic<Command> command_{Command::NONE};

    State state_;
    bool quantized_;
    bool closing_;
    bool erasing_;
    volatile bool erase_at_end_ = false; // an EraseAtEnd waiting for the loop point
    bool fading_out_ = false; // fading out after a pause or erase, still at play speed

    size_t length_;        // loop length in frames, once closed
    size_t write_pos_;     // frames recorded so far
    size_t play_pos_;      // playback position, whole frames
    float play_frac_;      // playback position, fraction of a frame
    size_t postroll_;      // frames of post-roll written so far
    size_t target_length_; // quantized: where the recording will close, 0 if not yet known
    uint32_t target_bars_; // quantized: the bars it will hold
    uint32_t beats_;       // a quantized loop's beats, once closed; 0 if not known
    float fade_;

    // speed, see StepSpeed(). The UI posts steps and resets, the audio applies them
    std::atomic<int> speed_steps_{0};
    std::atomic<int> semi_steps_{0};
    std::atomic<bool> speed_reset_{false};
    int semis_ = 0;
    bool reverse_ = false;
    volatile float speed_target_ = 1.f;
    float speed_ = 1.f;

    // the scramble: heard offset_ frames on, crossfading from from_offset_ for jump_left_ more
    size_t offset_ = 0, from_offset_ = 0;
    size_t jump_left_ = 0;

    // scrubbing while paused
    std::atomic<int> scrub_turns_{0};
    size_t scrub_count_ = 0;
    float scrub_, scrub_target_;

    uint32_t start_ticks_;
    uint32_t start_locks_ = 0; // the clock's locks at the record press
    uint32_t first_tick_time_;
    uint32_t first_tick_count_;
    bool have_first_tick_;
    TickFit fit_;          // the ticks since the press, for TickPeriod()
    double last_x_ = 0., last_y_ = 0.; // the fit's last point (ticks, samples since the first)
    bool last_late_ = false;           // and it came over 1.5 ticks' time after the one before
    uint32_t fit_ticks_ = 0; // the clock's tick count at the last point
};

} // namespace chompi
