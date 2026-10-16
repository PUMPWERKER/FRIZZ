/** @file TempoClock.h
 *  @brief The tempo and clock pulses for what follows it: the delay, the filter LFO, the
 *  freezer, the slicer, the tape stop and the scene morph. Three sources, in this order:
 *
 *  1. A loop (SetLoop): while one exists, it is the clock, whether MIDI clock runs or not.
 *     The loop holds a whole number of beats, and the pulses come from its play position, so
 *     beat 1 is the loop's start and the grid follows the transport: faster, slower,
 *     reversed (the positions then count down). While the loop is paused, the grid runs on
 *     by itself at that tempo, so the FX keep working on the live input; scrubbing doesn't
 *     move it, and on resume it snaps back to the loop's position at the next pulse. So
 *     does it at a standstill (the transport's hold zone, Looper::Standing, #174): there the
 *     tempo is the slowest speed's, which kMinBpm holds, and a morph lands on time. The
 *     tempo is the loop's times its speed. A quantized loop knows its beats; an
 *     unquantized one fits the tempo set before it (FitBeats) or, if none was, a guess at
 *     80-160 BPM (GuessBeats). A tap (Tap) refits the beats.
 *  2. MIDI clock: the tempo follows MidiClock rounded to whole BPM (so the delay time
 *     doesn't wobble with the clock's jitter), and pulses are counted from the incoming
 *     ticks, one pulse per 2 ticks (12 PPQN, what TEMPO's delay counts in). The whole BPM
 *     comes from the ticks counted over the last 2-4 s (FollowClock), which holds still under
 *     jitter that MidiClock's smoothed period doesn't (at 174 and 300 BPM, between two whole
 *     BPM), and moves only once the count is past the half by kTempoMargin; a jump in the
 *     tempo is followed at once, a new lock's once its ticks are counted a moment (0.1 s
 *     at 300 BPM, two ticks' time at 120).
 *  3. Neither: the last tempo is kept (120 BPM until one is set by a clock, a loop or taps)
 *     and pulses come from an internal phase at that tempo. A tap sets the tempo and puts a
 *     beat on the tap.
 *
 *  The clock factor (the settings page, MidiClock.h) times the tempo of each: a loop's, its
 *  beats as they were (a quantized one's as the clock was sent); the tapped and the last
 *  one; a clock's ticks count by it already. 50-300 BPM is the limit after it. A new factor
 *  keeps the grid in place: a loop's beat 1 on its start, a tapped beat on the tap's beats.
 *  At x1/2, a loop of an odd number of beats has a grid over two passes (SetLoopBeats).
 *
 *  Pulse() counts the pulses into one position, so every effect that follows the clock
 *  shares one grid: the 16ths, the delay's 8th-note edges (where it rolls its random events)
 *  and the bars are all counted from the same pulse. Without a loop, the count runs from
 *  power-on, or continues across a new MIDI clock lock; MIDI Start and Song Position aren't
 *  read.
 *
 *  Runs in the audio callback, once per block.
 */
#pragma once
#include <math.h>
#include "daisy.h"
#include "MidiClock.h"

namespace chompi
{

static const int kDefaultBpm = 120;
// 2 bars plus the delay's stereo offset must fit in its 10s buffer, which needs > 48 BPM
static const int kMinBpm = 50;
static const int kMaxBpm = 300;
static const uint32_t kPulsesPerBeat = 12;
static const uint32_t kTicksPerPulse = kTicksPerBeat / kPulsesPerBeat; // 24 PPQN MIDI ticks -> 12 PPQN pulses
static const uint32_t kPulsesPer16th = kPulsesPerBeat / 4;
static const uint32_t kPulsesPerEdge = kPulsesPerBeat / 2; // 8th notes
static const uint32_t kPulsesPerBar = kPulsesPerBeat * kBeatsPerBar;
// The pulse position wraps every 4 bars, a multiple of every grid the effects use
static const uint32_t kPulsesPerCycle = 4 * kPulsesPerBar;
// An unquantized loop with no tempo set before it is guessed into this range
static const float kGuessMinBpm = 80.f;
// From a clock: the whole BPM moves once the counted tempo is this far past the half
static const float kTempoMargin = .1f;
// From a clock: the ticks are counted over this long at least, twice this at most, in samples
static const uint32_t kTempoSpan = 2 * 48000;
// From a clock: before the count is this long, the smoothed period's tempo stands in for it
static const uint32_t kTempoMinSpan = 48000;
// From a clock's new lock: its ticks are counted this long at kMaxBpm before its tempo is
// taken, in proportion at a slower one (two ticks' time at least): the line through them is
// then as close as at 300 BPM
static const uint32_t kTempoLockSpan = 4800;

inline float ClampBpm(float bpm)
{
    return bpm < kMinBpm ? kMinBpm : (bpm > kMaxBpm ? kMaxBpm : bpm);
}

/** A loop of length frames, n beats long: its tempo */
inline float LoopBpm(size_t length, float sample_rate, uint32_t beats)
{
    return 60.f * beats * sample_rate / static_cast<float>(length);
}

/** The beats to give a loop nothing else knows the tempo of: 1, 2, 4, 8 ..., the first that
 *  puts it at kGuessMinBpm or above, so 80-160 BPM (faster only for loops under 0.75s) */
inline uint32_t GuessBeats(size_t length, float sample_rate)
{
    uint32_t beats = 1;
    while (LoopBpm(length, sample_rate, beats) < kGuessMinBpm)
        beats *= 2;
    return beats;
}

/** The whole number of beats that puts a loop closest to bpm, at least 1 */
inline uint32_t FitBeats(size_t length, float sample_rate, float bpm)
{
    const float beats = static_cast<float>(length) * bpm / (60.f * sample_rate);
    return beats < 1.5f ? 1 : static_cast<uint32_t>(beats + .5f);
}

/** The beats to give a loop from the card (#70), which knows none: a power of two, the one
 *  nearest bpm by ratio within kMinBpm-kMaxBpm; a guess (GuessBeats) if none is in it */
inline uint32_t NearestBeats(size_t length, float sample_rate, float bpm)
{
    uint32_t best = 0;
    float best_d = 0.f;
    for (uint32_t beats = 1; beats <= 1024; beats *= 2)
    {
        const float b = LoopBpm(length, sample_rate, beats);
        const float d = fmaxf(b / bpm, bpm / b); // the ratio's distance, either way
        if (b >= kMinBpm && b <= kMaxBpm && (!best || d < best_d))
        {
            best = beats;
            best_d = d;
        }
    }
    return best ? best : GuessBeats(length, sample_rate);
}

class TempoClock
{
public:
    void Init(float sample_rate, MidiClock* midi_clock)
    {
        sample_rate_ = sample_rate;
        midi_clock_ = midi_clock;
        factor_ = ClockFactor::ONE;
        scale_ = 1.f;
        bpm_ = kDefaultBpm;
        free_bpm_ = kDefaultBpm;
        tempo_ = kDefaultBpm;
        tempo_set_ = false;
        phase_ = 0.f;
        had_clock_ = false;
        last_ticks_ = 0;
        pulse_count_ = 0;
        loop_length_ = 0;
        dir_ = 1;
    }

    /** A loop closed: it's the clock from now on, length frames holding beats beats. from:
     *  where it starts, 0-1, 1 its end running in reverse (a loop from the card, switched to
     *  by time or in reverse, Looper::SwitchedAt) */
    void SetLoop(size_t length, uint32_t beats, float from = 0.f)
    {
        SyncFactor();
        loop_length_ = length;
        SetLoopBeats(beats);
        pass_ = 0;
        loop_pos_ = from;
        if (from >= 1.f)
        {
            // at its end in reverse: the next block crosses its start's line (beat 1), down
            // from the last pass of the grid
            pass_ = grid_pulses_ != loop_pulses_ ? 1 : 0;
            loop_idx_ = 0;
        }
        else
        {
            // on a pulse (its start): one step before it, so the first block pulses it (beat 1);
            // between two, the one passed is past
            const float at = from * static_cast<float>(loop_pulses_);
            const uint32_t idx = PulseIndex(from);
            loop_idx_ = at == static_cast<float>(idx) ? (idx + grid_pulses_ - 1) % grid_pulses_ : idx;
        }
        paused_ = false;
    }

    /** The loop was erased: back to the MIDI clock, or to free running at the loop's tempo */
    void ClearLoop()
    {
        SyncFactor();
        // the free clock runs at the whole BPM the FX use, so their times stay on its grid
        SetFreeTempo(loop_bpm_);
        loop_length_ = 0;
        paused_ = false;
        had_clock_ = false; // a running clock locks again, from here
        counting_ = false;
        phase_ = 0.f;
        dir_ = 1;
    }

    inline bool HasLoop() const { return loop_length_ > 0; }
    /** The loop's length in pulses, its beats' worth; 0 without a loop */
    inline uint32_t LoopPulses() const { return HasLoop() ? loop_pulses_ : 0; }
    /** Whether a tempo was set (by a clock, a loop or taps), for fitting a new loop to */
    inline bool TempoSet() const { return tempo_set_; }
    /** The tempo without a loop and before the factor: the clock's as sent or the free one,
     *  for fitting a new loop to */
    inline float GetBpm() const { return bpm_; }
    /** The loop's tempo at 1x before the factor, or without one GetBpm: what a loop from the
     *  card is fitted to (NearestBeats) */
    inline float FitBpm() const { return HasLoop() ? loop_bpm_ : bpm_; }
    /** Whether a tap would be taken: with a loop always, otherwise only without MIDI clock */
    inline bool CanTap() const { return HasLoop() || !midi_clock_->HasClock(); }

    /** A tapped tempo: refits a loop's beats, or sets the free tempo with a beat now */
    void Tap(float bpm)
    {
        SyncFactor();
        if (bpm <= 0.f || !CanTap())
            return;
        if (HasLoop())
        {
            tempo_set_ = true;
            SetLoopBeats(FitBeats(loop_length_, sample_rate_, bpm));
            loop_idx_ = free_idx_ = PulseIndex(loop_pos_);
            return;
        }
        SetFreeTempo(bpm);
        // the next pulse lands now and starts the nearest beat
        const uint32_t beat = (pulse_count_ + kPulsesPerBeat / 2) / kPulsesPerBeat * kPulsesPerBeat;
        pulse_count_ = (beat + kPulsesPerCycle - 1) % kPulsesPerCycle;
        phase_ = 1.f;
    }

    /** The loop was taken back to its start (MIDI's Start, Looper::Follow): a grid over two
     *  passes starts again on its first, as the gear MIDI out sends Start to does */
    inline void Rewound() { rewound_ = true; }

    /** Call once per block, after the looper, with its position (0-1), actual speed and
     *  whether it's paused, when there's a loop. Returns how many pulses happened during it
     *  (usually 0 or 1). Out of line: inlined into the engine's it's 1 KB more code (#413) */
    __attribute__((noinline)) uint32_t Process(size_t size, float loop_pos = 0.f, float loop_speed = 1.f,
                     bool loop_paused = false)
    {
        SyncFactor();
        if (HasLoop())
            return ProcessLoop(size, loop_pos, loop_speed, loop_paused);

        uint32_t pulses = 0;
        const bool has_clock = midi_clock_->HasClock();

        if (has_clock)
        {
            // 0 between the first and second tick of a new lock: keep the last tempo, and
            // count its ticks from its first (FollowClock)
            const float bpm = midi_clock_->GetBpm();
            if (bpm > 0.f)
                FollowClock(bpm); // by the factor already, as the ticks are
            else
            {
                StartCount();
                settling_ = true;
            }

            const uint32_t ticks = midi_clock_->GetTicks();
            if (!had_clock_)
                last_ticks_ = ticks; // new lock: count from here
            pulses = (ticks - last_ticks_) / kTicksPerPulse;
            last_ticks_ += pulses * kTicksPerPulse;
            phase_ = 0.f;
        }
        else
        {
            counting_ = false;
            pulses = FreePulses(size, free_bpm_);
        }

        had_clock_ = has_clock;
        return pulses;
    }

    /** Count one pulse; returns the new position, 0..kPulsesPerCycle - 1 */
    uint32_t Pulse()
    {
        if (HasLoop() && paused_)
        {
            free_idx_ = (free_idx_ + 1) % grid_pulses_;
            loop_pulse_ = free_idx_;
            pulse_count_ = free_idx_ % kPulsesPerCycle;
            return pulse_count_;
        }
        if (HasLoop())
        {
            // pulse b sits where the position crosses b / loop_pulses_ (in its pass of the
            // grid): forward, that's the index entered; in reverse, the one left
            uint32_t crossed;
            if (dir_ > 0)
                crossed = loop_idx_ = (loop_idx_ + 1) % grid_pulses_;
            else
            {
                crossed = loop_idx_;
                loop_idx_ = (loop_idx_ + grid_pulses_ - 1) % grid_pulses_;
            }
            loop_pulse_ = crossed;
            pulse_count_ = crossed % kPulsesPerCycle;
            return pulse_count_;
        }
        pulse_count_ = (pulse_count_ + 1) % kPulsesPerCycle;
        return pulse_count_;
    }

    /** The pulses from the last one to the next bar line, for a scene morph (FxMorph.h): a
     *  multiple of kPulsesPerBar, or a loop's wrap. An estimate: the transport may change
     *  direction, pause or resume on the way */
    uint32_t PulsesToBarLine() const
    {
        if (!HasLoop())
            return kPulsesPerBar - pulse_count_ % kPulsesPerBar;
        if (!paused_ && dir_ < 0)
            return loop_idx_ % kPulsesPerBar + 1; // the next pulse crosses loop_idx_
        const uint32_t idx = paused_ ? free_idx_ : loop_idx_;
        const uint32_t to_bar = kPulsesPerBar - idx % kPulsesPerBar;
        const uint32_t to_wrap = grid_pulses_ - idx;
        return to_bar < to_wrap ? to_bar : to_wrap;
    }
    /** With a loop: the pulses from the last one to its end (the next wrap), the way it runs;
     *  an estimate, as PulsesToBarLine */
    uint32_t PulsesToLoopEnd() const
    {
        const uint32_t idx = (paused_ ? free_idx_ : loop_idx_) % loop_pulses_;
        return !paused_ && dir_ < 0 ? idx + 1 : loop_pulses_ - idx;
    }
    /** The pulses between two bar lines: a bar, or a loop shorter than one */
    inline uint32_t PulsesPerBarLine() const
    {
        return HasLoop() && grid_pulses_ < kPulsesPerBar ? grid_pulses_ : kPulsesPerBar;
    }
    /** Whether a position Pulse() returned is on a bar line: every loop wrap is one too */
    static inline bool IsBarLine(uint32_t pos) { return pos % kPulsesPerBar == 0; }
    /** The time between two pulses, in samples: at the loop's real rate while one plays (it
     *  can be slower than kMinBpm, which only limits the FX's tempo), else the tempo's */
    inline float PulseSamples() const
    {
        const float bpm = HasLoop() ? pulse_bpm_ : free_bpm_;
        return bpm > 0.f ? 60.f * sample_rate_ / (bpm * kPulsesPerBeat) : 1e9f;
    }
    /** The position the last Pulse() returned */
    inline uint32_t Position() const { return pulse_count_; }
    /** With a loop: the last Pulse()'s place in it, 0 at its start, up to its pulses - 1:
     *  where a pulse that's a multiple of 3 is a 16th, for MIDI out's Song Position */
    inline uint32_t LoopPulse() const { return loop_pulse_; }
    /** With a loop playing: the loop pulse the next one on a multiple of every will be, in
     *  the direction it plays (an estimate: it may turn, pause or jump on the way) */
    uint32_t NextLoopPulseOn(uint32_t every) const
    {
        if (dir_ < 0)
            return loop_idx_ - loop_idx_ % every; // the next pulse crosses loop_idx_
        const uint32_t next = (loop_idx_ + 1) % grid_pulses_;
        return (next + (every - next % every) % every) % grid_pulses_;
    }

    /** The FX's tempo, whole BPM */
    inline int GetTempo() const { return tempo_; }
    /** The FX's tempo for their times (delay, freezer, tape stop): a loop's exact one, so
     *  the echoes stay on its beats however long it is; else the whole BPM, by the factor
     *  (a half at x1/2) */
    inline float GetFxBpm() const { return HasLoop() ? loop_fx_bpm_ : free_bpm_; }
    /** Whether the pulses count down: a loop playing in reverse */
    inline bool Reverse() const { return HasLoop() && !paused_ && dir_ < 0; }

private:
    static inline float Scale(ClockFactor factor)
    {
        return factor == ClockFactor::HALF ? .5f : factor == ClockFactor::DOUBLE ? 2.f : 1.f;
    }

    /** Takes up a new factor (the settings page's, MidiClock.h): the tempo and the grid are
     *  the source's times it from here, and the position keeps its beat. A loop's grid is
     *  counted from its position again, so beat 1 stays its start; the free one is scaled
     *  around its last pulse, so a tapped beat stays on its beats. A clock's ticks count by
     *  the factor already (MidiClock::SetFactor), and FollowClock takes the new tempo */
    void SyncFactor()
    {
        const ClockFactor factor = midi_clock_->Factor();
        if (factor == factor_)
            return;
        const float ratio = Scale(factor) / scale_;
        factor_ = factor;
        scale_ = Scale(factor);
        if (HasLoop())
        {
            SetLoopBeats(loop_beats_);
            loop_idx_ = PulseIndex(loop_pos_);
            if (paused_)
                free_idx_ = ScalePosition(free_idx_, ratio, grid_pulses_);
        }
        else if (!midi_clock_->HasClock())
        {
            pulse_count_ = ScalePosition(pulse_count_, ratio, kPulsesPerCycle);
            SetFreeBpm();
        }
    }

    /** A pulse position plus the phase towards the next, times ratio, mod cycle: the new
     *  position, and phase_ the new one's */
    uint32_t ScalePosition(uint32_t pos, float ratio, uint32_t cycle)
    {
        const float at = (static_cast<float>(pos) + phase_) * ratio;
        const float whole = floorf(at);
        phase_ = at - whole;
        return static_cast<uint32_t>(whole) % cycle;
    }

    /** Without a loop: the tempo before the factor, rounded to whole BPM so the FX's times
     *  don't wobble. It may be past 50-300 by the factor: the FX's limit applies after it */
    void SetFreeTempo(float bpm)
    {
        const float min = .5f * kMinBpm, max = 2.f * kMaxBpm;
        bpm_ = floorf((bpm < min ? min : (bpm > max ? max : bpm)) + .5f);
        tempo_set_ = true;
        SetFreeBpm();
    }
    /** The FX's tempo from bpm_ and the factor: whole BPM, or a half at x1/2 */
    void SetFreeBpm()
    {
        free_bpm_ = ClampBpm(bpm_ * scale_);
        tempo_ = static_cast<int>(free_bpm_ + .5f);
    }

    /** From a clock: its tempo by the factor, to whole BPM for the FX, and as sent for
     *  fitting a new loop to (GetBpm) */
    void SetClockTempo(float bpm)
    {
        tempo_ = static_cast<int>(ClampBpm(bpm) + .5f);
        free_bpm_ = static_cast<float>(tempo_);
        // unclamped: x1 again after a clock at 280 x2 gives 280, not 150
        bpm_ = floorf(bpm + .5f) / scale_;
        tempo_set_ = true;
    }

    /** A clock's tempo to the FX, from its smoothed period's tempo (smoothed): the line
     *  through the ticks counted since the older of two starts kTempoSpan apart, so over
     *  2-4 s, whose error shrinks with the span (the ticks' jitter doesn't add up, a late
     *  tick pulls a line through hundreds only a little) where the smoothed period's grows
     *  with the tempo (block and USB frame jitter on a short period). A smoothed tempo
     *  further off than its jitter is a new tempo: followed at once, and counted anew.
     *  A new lock's smoothed period starts from one interval, a block or a USB frame off (at
     *  120 BPM 122 or 119, at 300 up to 20 BPM off, #413): its first second follows the line
     *  through its ticks from its first (Process) instead, once that's long enough
     *  (kTempoLockSpan), the last tempo until then */
    void FollowClock(float smoothed)
    {
        const uint32_t ticks = midi_clock_->GetTicks();
        const uint32_t at = midi_clock_->GetLastTickTime();
        const uint32_t locks = midi_clock_->GetLocks();
        if (counting_ && locks == count_locks_ && ticks != count_last_)
        {
            count_[0].Add(ticks, at);
            count_[1].Add(ticks, at);
            count_last_ = ticks;
        }
        const uint32_t span = at - count_[0].at0;
        if (settling_ && locks == count_locks_ && span < kTempoMinSpan)
        {
            // an odd number of ticks: a pair's (one held back, sent with the next) shift then
            // cancels out of the line, where an even number tilts it (4 ticks: 20 BPM off)
            const bool enough = span * static_cast<float>(kMaxBpm) >= kTempoLockSpan * smoothed;
            const uint32_t n = static_cast<uint32_t>(count_[0].n);
            if (n >= 3 && n % 2 == 1 && enough)
                FollowCount(CountedBpm());
            return;
        }
        const bool settled = settling_ && locks == count_locks_; // a new lock's first second ended
        settling_ = false;
        const float counted = span >= kTempoMinSpan ? CountedBpm() : smoothed;
        if (!counting_ || locks != count_locks_ || fabsf(smoothed - counted) > ClockJitter(counted))
        {
            StartCount();
            // the smoothed tempo not on the new lock's line yet (pairs of ticks keep it off for a
            // while): its line again, the tempo kept; else a new tempo, followed at once
            settling_ = settled;
            if (!settled)
                SetClockTempo(smoothed);
            return;
        }
        if (at - count_[1].at0 >= kTempoSpan)
        {
            count_[0] = count_[1];
            count_[1].Start(ticks, at);
        }
        FollowCount(counted);
    }

    /** The ticks counted anew from the last one */
    __attribute__((noinline)) void StartCount()
    {
        count_[0].Start(midi_clock_->GetTicks(), midi_clock_->GetLastTickTime());
        count_[1] = count_[0];
        count_locks_ = midi_clock_->GetLocks();
        count_last_ = count_[0].ticks0;
        counting_ = true;
    }

    /** The tempo of the line through the ticks counted (count_[0]) */
    __attribute__((noinline)) float CountedBpm() const
    {
        return 60.f * sample_rate_ / (kTicksPerBeat * count_[0].Period());
    }
    /** A counted tempo: the whole BPM moves to it once it's past the half by kTempoMargin */
    void FollowCount(float counted)
    {
        if (fabsf(ClampBpm(counted) - static_cast<float>(tempo_)) > .5f + kTempoMargin)
            SetClockTempo(counted);
    }

    /** How far a clock's smoothed tempo strays at bpm, at most, from a DAW over USB: its
     *  period is a few blocks at 300 BPM, so it grows with the tempo squared (3 BPM there) */
    static float ClockJitter(float bpm)
    {
        const float r = bpm / static_cast<float>(kMaxBpm);
        return 1.f + 4.f * r * r;
    }

    /** The internal phase advanced by a block at bpm: the pulses it crossed */
    uint32_t FreePulses(size_t size, float bpm)
    {
        phase_ += static_cast<float>(size) * bpm * kPulsesPerBeat / (60.f * sample_rate_);
        const uint32_t pulses = static_cast<uint32_t>(phase_);
        phase_ -= static_cast<float>(pulses);
        return pulses;
    }

    /** A loop's beats before the factor. At x1/2 an odd number of them is a beat and a half,
     *  two and a half ... a pass: the grid then runs over two passes (decided in #116), so
     *  the beats stay even and the clock MIDI out sends steady, and beat 1 is the loop's
     *  start every other pass */
    void SetLoopBeats(uint32_t beats)
    {
        loop_beats_ = beats < 1 ? 1 : beats;
        loop_pulses_ = factor_ == ClockFactor::HALF     ? loop_beats_ * kPulsesPerBeat / 2
                       : factor_ == ClockFactor::DOUBLE ? loop_beats_ * kPulsesPerBeat * 2
                                                        : loop_beats_ * kPulsesPerBeat;
        const bool two = factor_ == ClockFactor::HALF && loop_beats_ % 2 != 0;
        grid_pulses_ = two ? 2 * loop_pulses_ : loop_pulses_;
        if (!two)
            pass_ = 0;
        loop_bpm_ = LoopBpm(loop_length_, sample_rate_, loop_beats_);
    }

    inline uint32_t PulseIndex(float pos) const
    {
        const uint32_t idx = static_cast<uint32_t>(pos * loop_pulses_);
        return pass_ * loop_pulses_ + (idx < loop_pulses_ ? idx : loop_pulses_ - 1);
    }

    /** The pulses the play position crossed since the last block, in its direction; while
     *  paused, the pulses of the grid running on by itself */
    uint32_t ProcessLoop(size_t size, float pos, float speed, bool paused)
    {
        // a grid over two passes: the position wrapping, either way, is the other pass; taken
        // back to the start, its first, the next pulse beat 1 (as a new loop's)
        if (grid_pulses_ != loop_pulses_ && rewound_)
        {
            pass_ = 0;
            loop_idx_ = grid_pulses_ - 1;
        }
        else if (!paused && grid_pulses_ != loop_pulses_ && fabsf(pos - loop_pos_) > .5f)
            pass_ ^= 1;
        rewound_ = false;
        loop_pos_ = pos;
        const float bpm = ClampBpm(loop_bpm_ * scale_ * fabsf(speed));
        tempo_ = static_cast<int>(bpm + .5f);
        loop_fx_bpm_ = bpm;
        had_clock_ = false;
        // the pulses' real rate: paused, the grid's own; playing, the loop's, unclamped
        pulse_bpm_ = paused ? bpm : loop_bpm_ * scale_ * fabsf(speed);

        if (paused)
        {
            if (!paused_)
            {
                // on from where the loop stopped, the next pulse when it would have come
                paused_ = true;
                free_idx_ = loop_idx_;
                const float at = pos * loop_pulses_;
                phase_ = at - floorf(at);
            }
            return FreePulses(size, bpm);
        }
        if (paused_)
        {
            // resumed: back on the loop's position, which the next pulse brings
            paused_ = false;
            loop_idx_ = PulseIndex(pos);
        }

        const int32_t pulses = static_cast<int32_t>(grid_pulses_);
        int32_t delta = static_cast<int32_t>(PulseIndex(pos)) - static_cast<int32_t>(loop_idx_);
        // across the loop point: the short way round
        if (delta > pulses / 2)
            delta -= pulses;
        else if (delta < -pulses / 2)
            delta += pulses;
        // kept between pulses: a block that crosses none says nothing about the direction
        if (delta != 0)
            dir_ = delta < 0 ? -1 : 1;
        return static_cast<uint32_t>(delta < 0 ? -delta : delta);
    }

    float sample_rate_;
    MidiClock* midi_clock_;
    ClockFactor factor_; // the clock factor the tempo is by (SyncFactor)
    float scale_;        // and as a multiple
    float bpm_;      // without a loop: the clock's as sent or the free tempo, before the factor
    float free_bpm_; // without a loop: what the FX get, by the factor
    int tempo_;      // what the FX get, whole BPM
    bool tempo_set_;
    float phase_;
    bool had_clock_;
    uint32_t last_ticks_;
    uint32_t pulse_count_; // the position, mod kPulsesPerCycle

    /** A least-squares line through the ticks' arrival times since a start: the period */
    struct TickLine
    {
        uint32_t ticks0, at0;
        double n, x, y, xy, xx; // sums of the ticks and times since the start

        void Start(uint32_t ticks, uint32_t at)
        {
            ticks0 = ticks;
            at0 = at;
            n = 1.;
            x = y = xy = xx = 0.;
        }
        void Add(uint32_t ticks, uint32_t at)
        {
            const double dx = static_cast<double>(ticks - ticks0);
            const double dy = static_cast<double>(at - at0);
            n += 1.;
            x += dx;
            y += dy;
            xy += dx * dy;
            xx += dx * dx;
        }
        /** Samples per tick */
        float Period() const { return static_cast<float>((n * xy - x * y) / (n * xx - x * x)); }
    };

    // a clock's ticks counted for its tempo (FollowClock): from two starts, the older first
    bool counting_ = false;
    bool settling_ = false;   // counted from a new lock's first tick, for up to kTempoMinSpan
    uint32_t count_locks_ = 0;
    uint32_t count_last_ = 0; // the ticks at the last block that had any
    TickLine count_[2];

    // the loop, while there is one (loop_length_ > 0)
    size_t loop_length_;
    uint32_t loop_beats_;
    uint32_t loop_pulses_; // per pass
    uint32_t grid_pulses_; // the grid's, over one pass or two (SetLoopBeats)
    uint32_t pass_ = 0;    // which of the two the position is in
    bool rewound_ = false; // Rewound() since the last block
    float loop_bpm_;       // at 1x, before the factor
    float loop_pos_ = 0.f;
    uint32_t loop_idx_;    // the pulse interval the position was last counted in
    int32_t dir_;          // the pulses' direction, -1 in reverse
    bool paused_ = false;  // the loop is paused: the grid runs on by itself
    float pulse_bpm_ = kDefaultBpm; // the loop's pulses' real rate (PulseSamples)
    float loop_fx_bpm_ = kDefaultBpm; // the loop's tempo at its speed, clamped (GetFxBpm)
    uint32_t free_idx_ = 0; // while paused, the grid's last pulse
    uint32_t loop_pulse_ = 0; // the last pulse's place in the loop (LoopPulse)
};

} // namespace chompi
