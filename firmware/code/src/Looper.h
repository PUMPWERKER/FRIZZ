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
 *  The semitones run through into reverse (#174): below 1/16x a hold zone of kStillSteps
 *  steps, where the tape glides to a standstill (0x) and stands (Standing), then reverse from
 *  1/16x. Below 1/16x the loop's level follows its speed, so the glide into the standstill and
 *  out of it sounds as a tape stop and a stopped read head gives no DC; the engine's clock
 *  takes the standstill as a pause (TempoClock.h).
 *
 *  The soft limit (#152): the loop is int16, so what's written is held under kLoopCeiling by a
 *  peak limiter, stereo-linked, rather than clipped at full scale: it takes a peak at once,
 *  holds kLoopHoldFrames (a period of 47 Hz, so it doesn't ripple on a steady tone) and lets go
 *  over some 150 ms. Below the ceiling it isn't there, sample for sample; a recording starts it
 *  afresh. Live the input is float up to the safety limiter, so without it a hot input sounded
 *  clean live and clipped in the loop (tools/loopclip.py).
 *
 *  Scramble (the chaos key, FxChaos.h): the loop is heard from a number of frames on from the
 *  read head, which itself runs on as ever, so the loop's position, the FX clock and its end
 *  don't move. A new offset crossfades from the old over kXfadeFrames.
 *
 *  A loop from the card (#70, LoopControls.h) takes over with Switch: the looper then reads
 *  another buffer, which holds the loop's frames and its head again after its end, so the
 *  post-roll's crossfade at the loop point is the loop's own. It switches at once (the next
 *  block), at the loop point once Wraps() has counted to a pass, or when the engine says
 *  (ReleaseSwitch: a scene sequence's landing). The tape runs on across it: the new loop
 *  keeps the speed and the direction, standing if the tape stands. It starts at its start
 *  in the direction the tape runs (its end in reverse), or, by time (Switch's keep), as far
 *  into its pass as the loop it leaves was into its own, modulo its length: two loops at one
 *  tempo stay on the beat, whatever their lengths. Heard, the loop it leaves (the other
 *  voice) fades out over kSwitchFrames from where it is, so a switch at the loop point,
 *  where the old loop's head is its natural continuation, is gapless. A recording always
 *  goes to the looper's own buffer (Init's), whichever it read before.
 *
 *  The crossfader (#70: SHIFT + a scene key, the transport turned, a white key) plays two
 *  loops: Switch with kSwitchFade starts the new one as the other voice, by time, at the
 *  tape's speed, and Fade sets the mix, 0 this loop to 1 the other, equal power, glided over
 *  kSwitchFrames from end to end. The loop, its clock and Wraps() stay this one's until
 *  EndFade: then the mix glides to the side that won, and the other loop takes over where
 *  it is (a switch, Switches()) or goes.
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
static const size_t kSwitchFrames = 480;   // 10 ms: the crossfade of a switch to a loaded loop

// The soft limit: what's written stays under this, full scale 1 (see the file comment)
static const float kLoopCeiling = .95f;
static const uint32_t kLoopHoldFrames = 1024;
static const float kLoopRelease = 1.f - 1.f / 7200.f; // a frame's, ~150 ms

// Speed ladder, in semitones: octaves (12k) and fifths above them (12k + 7), as TAPE.
// Top is 2x (+12), bottom is 1/16x (-48); a ladder step below the bottom flips direction, a
// semitone step stops there.
static const int kSpeedMaxSemis = 12;
static const int kSpeedMinSemis = -48;
static const float kSpeedSlewCoeff = .0001f; // TAPE's tape-slew glide, ~0.2s
// The hold zone between 1/16x and -1/16x (#174): semitone steps at a standstill, 2 detents
// each, so about 4 detents of the transport knob
static const int kStillSteps = 2;

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
        rec_mem_ = mem;
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

    /** One semitone, turning as StepSpeed; stops at 2x, and runs on from 1/16x through the
     *  hold zone (Standing) into the other direction at 1/16x */
    void StepSemitone(int dir) { semi_steps_.fetch_add(dir > 0 ? 1 : (dir < 0 ? -1 : 0)); }

    /** Back to 1x forward, at the next audio block; steps posted before it are dropped */
    void ResetSpeed()
    {
        speed_steps_.store(0);
        semi_steps_.store(0);
        speed_reset_.store(true);
    }

    /** The speed in semitones outright, -48 (1/16x) to 12 (2x), in the direction it runs:
     *  MIDI's (CC 90). Taken up at the next audio block, like the steps */
    void SetSemitones(int semis) { semis_to_.store(semis); }

    /** MIDI's transport, while following it (MidiControl.h, MANUAL.md "Start and Stop"): from
     *  the audio callback, before Process. msg is the status byte: 0xFA Start, 0xFB Continue,
     *  0xFC Stop, 0xF2 Song Position (spp 16ths). tick: MidiClock::GetLastTick as it came.
     *  A loop recorded to the clock (GetBeats) is kept in step: Start takes it back to its
     *  start, and it plays from the first tick after Start or Continue; paused, Song Position
     *  moves it there, by its length in the sender's 16ths. A free loop plays and pauses as PLAY does */
    void Follow(uint8_t msg, uint16_t spp, uint32_t tick)
    {
        if (erasing_ || (state_ != State::PLAYING && state_ != State::PAUSED))
            return;
        const bool clocked = beats_ > 0;
        if (msg == 0xFC)
        {
            cue_ = false;
            if (state_ == State::PLAYING)
                Pause();
        }
        else if (msg == 0xF2)
        {
            // the sender's 16ths: the loop's beats are FRIZZ's, by the factor it was recorded at
            const uint32_t sixteenths = GetSenderBeats() * 4;
            if (clocked && state_ == State::PAUSED)
                JumpTo(static_cast<size_t>(static_cast<uint64_t>(spp % sixteenths) * length_ / sixteenths));
        }
        else if (clocked && (msg == 0xFA || state_ == State::PAUSED))
        {
            // from the next tick: Start from the loop's start (playing, it jumps there then)
            cue_ = true;
            cue_tick_ = tick;
            cue_start_ = msg == 0xFA;
        }
        else if (state_ == State::PAUSED)
            Play();
    }

    /** How many times Follow's Start took the playing loop back to its start: for MIDI out,
     *  which sends Start then too */
    inline uint32_t Rewinds() const { return rewinds_; }

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

    // ===== a loop from the card (#70): from MainLoop with the audio interrupt blocked =====

    static const uint32_t kSwitchNow = 0xFFFFFFFFu;  // Switch's at: the next block
    static const uint32_t kSwitchHeld = 0xFFFFFFFEu; // until ReleaseSwitch
    static const uint32_t kSwitchFade = 0xFFFFFFFDu; // beside this one, on the crossfader

    /** The loop of length frames in mem (int16 stereo, its first kXfadeFrames again after its
     *  end) takes over at: kSwitchNow, kSwitchHeld, or the loop point the read head crosses
     *  once Wraps() has reached it; or kSwitchFade, the crossfader's (file comment). beats: its
     *  own, 0 for none known (the engine guesses). keep: by time, as far into its pass as this
     *  one is (playing), else from its start. Replaces a switch waiting; none while recording
     *  or erasing */
    void Switch(const int16_t* mem, size_t length, uint32_t beats, uint32_t at, bool keep = false)
    {
        next_mem_ = mem;
        next_length_ = length;
        next_beats_ = beats;
        next_at_ = at;
        next_keep_ = keep;
    }
    inline void CancelSwitch() { next_mem_ = nullptr; }
    /** A held switch goes at the next block */
    inline void ReleaseSwitch()
    {
        if (next_mem_ && next_at_ == kSwitchHeld)
            next_at_ = kSwitchNow;
    }
    inline bool SwitchHeld() const { return next_mem_ && next_at_ == kSwitchHeld; }
    inline bool SwitchWaiting() const { return next_mem_ != nullptr; }
    /** The crossfader's mix, 0 this loop to 1 the other (kSwitchFade's), while it has one */
    inline void Fade(float t)
    {
        if (fader_ && !end_)
            mix_to_ = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    }
    /** The crossfader let go: take, the other loop takes over (from the middle on); else it
     *  goes. A kSwitchFade still waiting: at once, or not at all */
    void EndFade(bool take)
    {
        if (next_mem_ && next_at_ == kSwitchFade)
        {
            next_at_ = kSwitchNow;
            if (!take)
                next_mem_ = nullptr;
        }
        if (!fader_ || end_)
            return; // decided already
        mix_to_ = take ? 1.f : 0.f;
        end_ = true;
    }
    /** Two loops on the crossfader (or one waiting to join it) */
    inline bool Fading() const { return fader_ || (next_mem_ && next_at_ == kSwitchFade); }
    /** Whether the buffer at buf is read now or will be: the loop's, the other one, the one
     *  waiting, each starting within its first sector (a WAV's data after its header) */
    inline bool Reads(const void* buf) const
    {
        return (state_ != State::EMPTY && In(mem_, buf)) || In(other_mem_, buf) || In(next_mem_, buf);
    }
    static inline bool In(const int16_t* loop, const void* buf)
    {
        const uintptr_t at = reinterpret_cast<uintptr_t>(loop) - reinterpret_cast<uintptr_t>(buf);
        return loop && at < 512;
    }
    /** The loop points crossed playing, counted since power-on */
    inline uint32_t Wraps() const { return wraps_; }
    /** One more for each switch done */
    inline uint32_t Switches() const { return switches_; }
    /** The loop is a loaded one (Switch), with these beats (0: none known) */
    inline bool Loaded() const { return mem_ != rec_mem_; }
    inline uint32_t LoadedBeats() const { return loaded_beats_; }
    /** Where the last switch's loop started, 0-1 through it: for the clock (TempoClock::SetLoop) */
    inline float SwitchedAt() const { return switched_at_; }

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

    /** The speed's semitones, -48 to 12, either way it runs (MIDI feedback's CC 90); at a
     *  standstill -48 */
    inline int GetSemitones() const { return Standing() ? kSpeedMinSemis : semis_; }
    /** In the hold zone between 1/16x and -1/16x: gliding to a standstill, or standing */
    inline bool Standing() const { return semis_ < kSpeedMinSemis; }

    /** Current speed target: negative is reverse, 1 is the recorded speed */
    inline float GetSpeed() const { return speed_target_; }

    /** How far on from the read head the loop is heard (Scramble), in frames */
    inline size_t GetScramble() const { return offset_; }
    /** Loop length in frames, 0 until a recording closes */

    inline size_t GetLength() const { return length_; }
    /** Beats in a quantized loop, from the clock it was recorded to; 0 for an unquantized
     *  loop, or one closed early because the clock stopped */
    inline uint32_t GetBeats() const { return beats_; }
    /** GetBeats as the clock was sent: the ticks it was recorded to counted by the clock
     *  factor then (MidiClock.h), at half every other one, at double each twice */
    inline uint32_t GetSenderBeats() const
    {
        return rec_factor_ == ClockFactor::HALF     ? beats_ * 2
               : rec_factor_ == ClockFactor::DOUBLE ? beats_ / 2
                                                    : beats_;
    }
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
        if (__builtin_expect(next_mem_ != nullptr, 0))
            Switched();
        if (__builtin_expect(other_mem_ != nullptr, 0))
            Settle();
        HandleSpeed();
        if (__builtin_expect(cue_, 0))
            HandleCue();

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
                    // and to a standstill, in denormals (#174): onto 0 within a hair
                    const float was = speed_;
                    daisysp::fonepole(speed_, speed_target_, kSpeedSlewCoeff);
                    if (speed_ == was || (speed_target_ == 0.f && fabsf(speed_) < 1e-5f))
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
                    {
                        // onto its target (#168): on its own it stalls ~3e-5 short of 1,
                        // where a step rounds to nothing (as the speed's glide), or runs into
                        // denormals towards 0, which a hair's snap ends
                        const float was = fade_;
                        daisysp::fonepole(fade_, fade_target, kPlayFadeCoeff);
                        if (fade_ == was || fabsf(fade_ - fade_target) < 1e-5f)
                            fade_ = fade_target;
                    }

                    // the fade-out after a pause or erase keeps moving at play speed, so it
                    // doesn't freeze on one sample (a DC thump)
                    if (fading_out_ && (fade_ <= .0001f || scrub_target_ != 0.f))
                        fading_out_ = false;

                    if (fade_ > .0001f)
                    {
                        float gain = fade_;
                        if (__builtin_expect(quiet_, 0))
                            gain *= StillGain();
                        const float step = playing || fading_out_ ? speed_ : scrub_;
                        if (__builtin_expect((offset_ | jump_left_ | reinterpret_cast<uintptr_t>(other_mem_)) != 0, 0))
                            ReadScrambled(&out_l[i], &out_r[i], step);
                        else
                            ReadInterpolated(play_pos_, &out_l[i], &out_r[i]);
                        out_l[i] *= gain;
                        out_r[i] *= gain;
                        // an erase or a switch waiting for the loop point happens there
                        if (Advance(step) && playing)
                            Wrapped();
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
        // out of the standstill: 1/16x the way it turns, right forward
        if (Standing())
        {
            reverse_ = dir < 0;
            SetSemis(kSpeedMinSemis);
            return;
        }
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

    /** One semitone, see StepSemitone: below 1/16x the hold zone, past it the other way */
    void ApplySemitone(int dir)
    {
        const int semis = semis_ + (((dir > 0) != reverse_) ? 1 : -1);
        if (semis > kSpeedMaxSemis)
            return;
        if (semis < kSpeedMinSemis - kStillSteps)
        {
            reverse_ = !reverse_; // through the standstill
            SetSemis(kSpeedMinSemis);
        }
        else
            SetSemis(semis);
    }

    /** semis below kSpeedMinSemis: the hold zone, standing */
    void SetSemis(int semis)
    {
        semis_ = semis;
        if (semis < kSpeedMinSemis)
        {
            speed_target_ = 0.f;
            quiet_ = true;
            return;
        }
        speed_target_ = (reverse_ ? -1.f : 1.f) * powf(2.f, semis_ / 12.f);
    }

    /** Around the standstill (quiet_): the loop's level follows its speed below 1/16x, until
     *  it's back up to that out of the hold zone */
    inline float StillGain()
    {
        const float gain = fabsf(speed_) * 16.f;
        if (!Standing() && (gain >= 1.f || speed_ == speed_target_))
        {
            quiet_ = false;
            return 1.f;
        }
        return gain < 1.f ? gain : 1.f;
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
        const int to = semis_to_.exchange(kNoSemis);
        if (to != kNoSemis)
            SetSemis(to < kSpeedMinSemis ? kSpeedMinSemis : (to > kSpeedMaxSemis ? kSpeedMaxSemis : to));
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
            rec_peak_ = 0.f;
            rec_hold_ = 0;
            target_length_ = 0;
            target_bars_ = 0;
            start_ticks_ = midi_clock_->GetTicks();
            start_locks_ = midi_clock_->GetLocks();
            // a new factor is a new lock (GetLocks): it closes the loop unquantized
            rec_factor_ = midi_clock_->Factor();
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
            cue_ = false;
            if (state_ == State::PLAYING)
                Pause();
            else if (state_ == State::PAUSED)
                Play();
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

    void Pause()
    {
        ResetScrub();
        state_ = State::PAUSED;
        fading_out_ = true;
    }
    void Play()
    {
        ResetScrub();
        state_ = State::PLAYING;
        fading_out_ = false;
    }
    /** A scrub left over from the last pause mustn't carry into the next one */
    void ResetScrub()
    {
        scrub_turns_.store(0);
        scrub_ = scrub_target_ = 0.f;
        scrub_count_ = 0;
    }

    /** Follow's cue: the first tick after it (or none coming any more) plays the loop, from
     *  its start for a Start */
    __attribute__((noinline, cold)) void HandleCue()
    {
        if (erasing_ || state_ == State::EMPTY)
        {
            cue_ = false;
            return;
        }
        if (midi_clock_->HasClock() && midi_clock_->GetLastTick() == cue_tick_)
            return;
        cue_ = false;
        if (cue_start_)
        {
            if (state_ == State::PLAYING)
                rewinds_++;
            JumpTo(0);
        }
        if (state_ == State::PAUSED)
            Play();
    }

    /** The read head to frame pos. While it's heard, the scramble's crossfade glides from
     *  where it was, so the jump doesn't click */
    void JumpTo(size_t pos)
    {
        if (pos >= length_)
            pos = 0;
        const size_t heard = Shifted(offset_);
        play_pos_ = pos;
        play_frac_ = 0.f;
        if (fade_ > .0001f)
        {
            from_offset_ = heard >= pos ? heard - pos : heard + length_ - pos;
            jump_left_ = kXfadeFrames;
        }
    }

    /** The read head crossed the loop point, playing: an erase waiting for it starts its fade,
     *  or a switch waiting for this pass goes */
    __attribute__((noinline, cold)) void Wrapped()
    {
        wraps_++;
        if (erase_at_end_)
            BeginErase();
        else if (next_mem_ && next_at_ < kSwitchFade && wraps_ >= next_at_)
            DoSwitch();
    }

    /** A block's start with a switch waiting: at once, or the crossfader's other loop once
     *  the voice is free (a switch's fade out over) */
    __attribute__((noinline, cold)) void Switched()
    {
        if (next_at_ == kSwitchNow)
            DoSwitch();
        else if (next_at_ == kSwitchFade && !other_mem_)
        {
            // nothing to fade from: at once
            if (state_ != State::PLAYING && state_ != State::PAUSED)
            {
                DoSwitch();
                return;
            }
            if (erasing_)
                return;
            other_mem_ = next_mem_;
            other_length_ = next_length_;
            other_beats_ = next_beats_;
            next_mem_ = nullptr;
            ByTime(other_length_, other_pos_, other_frac_);
            mix_ = mix_to_ = 0.f;
            fader_ = true;
            end_ = false;
        }
    }

    /** Where a loop of length frames starts by time (Switch's keep): as far into its pass,
     *  the way the tape runs, as this one is into its own, modulo its length */
    void ByTime(size_t length, size_t& pos, float& frac) const
    {
        frac = play_frac_;
        if (!reverse_)
        {
            pos = play_pos_ % length;
            return;
        }
        // in reverse a pass runs from the end down: the frames left to its start alike
        const size_t left = (length_ - play_pos_) % length;
        pos = length - (left ? left : length);
    }

    /** The loop waiting (Switch) takes over: by time, or at its start the way the tape runs;
     *  the one heard fades out over kSwitchFrames from where it is (the other voice). The
     *  tape keeps its speed and direction. From empty it plays, fading in; paused it stays
     *  paused */
    __attribute__((noinline, cold)) void DoSwitch()
    {
        const int16_t* mem = next_mem_;
        next_mem_ = nullptr;
        if (state_ == State::RECORDING || erasing_)
            return;
        const bool playing = state_ == State::PLAYING;
        size_t pos = 0;
        float frac = 0.f;
        if (next_keep_ && playing)
            ByTime(next_length_, pos, frac);
        else if (reverse_)
            pos = next_length_ - 1;
        other_mem_ = nullptr;
        fader_ = end_ = false;
        if (playing && fade_ > .0001f)
        {
            other_mem_ = mem_;
            other_length_ = length_;
            other_pos_ = Shifted(offset_);
            other_frac_ = play_frac_;
            mix_ = 1.f;
            mix_to_ = 0.f;
        }
        if (!playing)
        {
            fade_ = 0.f;
            fading_out_ = false;
            ResetScrub();
        }
        if (state_ == State::EMPTY)
            state_ = State::PLAYING;
        TakeOver(mem, next_length_, next_beats_, pos, frac);
    }

    /** The loop in mem from pos: the looper's from here, a switch (Switches) */
    void TakeOver(const int16_t* mem, size_t length, uint32_t beats, size_t pos, float frac)
    {
        mem_ = mem;
        length_ = length;
        loaded_beats_ = beats;
        beats_ = 0;
        rec_factor_ = ClockFactor::ONE;
        postroll_ = kXfadeFrames;
        play_pos_ = pos;
        play_frac_ = frac;
        switched_at_ = static_cast<float>(pos) / static_cast<float>(length);
        offset_ = from_offset_ = 0;
        jump_left_ = 0;
        cue_ = false;
        erase_at_end_ = false;
        switches_++;
    }

    /** A block's start with the other voice: its fade done, it goes, or, the crossfader's
     *  won, it takes over where it is */
    __attribute__((noinline, cold)) void Settle()
    {
        if (fade_ <= .0001f)
            mix_ = mix_to_; // unheard (paused): nothing to glide
        if (mix_ != mix_to_ || (fader_ && !end_))
            return;
        const int16_t* mem = other_mem_;
        other_mem_ = nullptr;
        if (fader_ && mix_ >= 1.f && state_ != State::EMPTY && !erasing_)
            TakeOver(mem, other_length_, other_beats_, other_pos_, other_frac_);
        fader_ = end_ = false;
        mix_ = mix_to_ = 0.f;
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
        mem_ = rec_mem_;
        next_mem_ = nullptr;
        other_mem_ = nullptr;
        fader_ = end_ = false;
        mix_ = mix_to_ = 0.f;
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
        semis_to_.store(kNoSemis);
        speed_reset_.store(false);
        cue_ = false;
        speed_ = 1.f;
        quiet_ = false;
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

    /** The read head offset_ on, crossfading from from_offset_ while jump_left_ runs, and
     *  mixed with the other voice while there is one (a switch's fade out, the crossfader's
     *  loop), which moves step frames a sample as the read head does */
    __attribute__((noinline, cold)) void ReadScrambled(float* l, float* r, float step)
    {
        ReadInterpolated(Shifted(offset_), l, r);
        if (jump_left_)
        {
            float ol, or_;
            ReadInterpolated(Shifted(from_offset_), &ol, &or_);
            const float old = static_cast<float>(jump_left_) / kXfadeFrames;
            *l += (ol - *l) * old;
            *r += (or_ - *r) * old;
            jump_left_--;
        }
        if (other_mem_)
        {
            // the mix: a switch's fade linear, the crossfader's equal power
            const float x = mix_;
            const float go = fader_ ? EqualPower(x) : x, gm = fader_ ? EqualPower(1.f - x) : 1.f - x;
            if (x != mix_to_)
            {
                const float to = mix_to_, d = 1.f / kSwitchFrames;
                mix_ = to > x ? (x + d < to ? x + d : to) : (x - d > to ? x - d : to);
            }
            float ol, or_;
            ReadAt(other_mem_, other_length_, other_pos_, other_frac_, &ol, &or_);
            *l = *l * gm + ol * go;
            *r = *r * gm + or_ * go;
            other_frac_ += step;
            while (other_frac_ >= 1.f)
            {
                other_frac_ -= 1.f;
                if (++other_pos_ >= other_length_)
                    other_pos_ = 0;
            }
            while (other_frac_ < 0.f)
            {
                other_frac_ += 1.f;
                other_pos_ = other_pos_ ? other_pos_ - 1 : other_length_ - 1;
            }
        }
    }
    /** sin(x pi/2) within 1 %, 0 at 0 and 1 at 1: the crossfader's gains, x and 1 - x */
    static inline float EqualPower(float x) { return x * (1.5707963f - .5707963f * x * x); }
    /** A loop of length frames in mem, at pos and frac, as ReadInterpolated (the other voice's:
     *  a loaded loop, no post-roll) */
    static void ReadAt(const int16_t* mem, size_t length, size_t pos, float frac, float* l, float* r)
    {
        float xl[4], xr[4];
        for (int k = 0; k < 4; k++)
        {
            size_t f = pos + length + static_cast<size_t>(k) - 1;
            while (f >= length)
                f -= length;
            xl[k] = s162f(mem[f * 2]);
            xr[k] = s162f(mem[f * 2 + 1]);
        }
        *l = frac == 0.f ? xl[1] : Hermite(xl, frac);
        *r = frac == 0.f ? xr[1] : Hermite(xr, frac);
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
        // the soft limit: a peak at once, held, then let go; a gain only above the ceiling
        const float peak = fmaxf(fabsf(l), fabsf(r));
        if (peak >= rec_peak_)
        {
            rec_peak_ = peak;
            rec_hold_ = kLoopHoldFrames;
        }
        else if (rec_hold_)
            rec_hold_--;
        else
            rec_peak_ *= kLoopRelease;
        if (__builtin_expect(rec_peak_ > kLoopCeiling, 0))
        {
            const float g = kLoopCeiling / rec_peak_;
            l *= g;
            r *= g;
        }
        rec_mem_[frame * 2] = static_cast<int16_t>(f2s16(l));
        rec_mem_[frame * 2 + 1] = static_cast<int16_t>(f2s16(r));
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

    const int16_t* mem_;     // the loop read: the recording's, or a loaded one (Switch)
    int16_t* rec_mem_;       // the recording's, written
    MidiClock* midi_clock_;
    // a switch waiting (Switch)
    const int16_t* next_mem_ = nullptr;
    size_t next_length_ = 0;
    uint32_t next_beats_ = 0, next_at_ = 0, loaded_beats_ = 0;
    bool next_keep_ = false;
    float switched_at_ = 0.f; // where the last one started, 0-1 (SwitchedAt)
    // the other voice: the loop a switch left, fading out, or the crossfader's (Fade), mixed
    // at mix_ (0 this loop, 1 the other) gliding to mix_to_; end_: the crossfader let go
    const int16_t* other_mem_ = nullptr;
    size_t other_length_ = 0, other_pos_ = 0;
    float other_frac_ = 0.f;
    uint32_t other_beats_ = 0;
    volatile float mix_to_ = 0.f;
    float mix_ = 0.f;
    bool fader_ = false, end_ = false;
    uint32_t wraps_ = 0, switches_ = 0;

    std::atomic<Command> command_{Command::NONE};

    State state_;
    bool quantized_;
    bool closing_;
    bool erasing_;
    volatile bool erase_at_end_ = false; // an EraseAtEnd waiting for the loop point
    bool cue_ = false;       // Follow's: plays at the first tick after cue_tick_
    bool cue_start_ = false; // and from the start
    uint32_t cue_tick_ = 0;
    uint32_t rewinds_ = 0;
    bool fading_out_ = false; // fading out after a pause or erase, still at play speed

    size_t length_;        // loop length in frames, once closed
    size_t write_pos_;     // frames recorded so far
    size_t play_pos_;      // playback position, whole frames
    float play_frac_;      // playback position, fraction of a frame
    size_t postroll_;      // frames of post-roll written so far
    float rec_peak_ = 0.f; // the soft limit's peak, held for rec_hold_ more frames (Write)
    uint32_t rec_hold_ = 0;
    size_t target_length_; // quantized: where the recording will close, 0 if not yet known
    uint32_t target_bars_; // quantized: the bars it will hold
    uint32_t beats_;       // a quantized loop's beats, once closed; 0 if not known
    float fade_;

    // speed, see StepSpeed(). The UI posts steps and resets, the audio applies them
    std::atomic<int> speed_steps_{0};
    std::atomic<int> semi_steps_{0};
    std::atomic<bool> speed_reset_{false};
    static const int kNoSemis = -1000;
    std::atomic<int> semis_to_{kNoSemis}; // SetSemitones', kNoSemis for none
    int semis_ = 0;
    bool reverse_ = false;
    volatile float speed_target_ = 1.f;
    float speed_ = 1.f;
    bool quiet_ = false; // around a standstill: the level follows the speed (StillGain)

    // the scramble: heard offset_ frames on, crossfading from from_offset_ for jump_left_ more
    size_t offset_ = 0, from_offset_ = 0;
    size_t jump_left_ = 0;

    // scrubbing while paused
    std::atomic<int> scrub_turns_{0};
    size_t scrub_count_ = 0;
    float scrub_, scrub_target_;

    uint32_t start_ticks_;
    uint32_t start_locks_ = 0; // the clock's locks at the record press
    ClockFactor rec_factor_ = ClockFactor::ONE; // and its factor (GetSenderBeats)
    uint32_t first_tick_time_;
    uint32_t first_tick_count_;
    bool have_first_tick_;
    TickFit fit_;          // the ticks since the press, for TickPeriod()
    double last_x_ = 0., last_y_ = 0.; // the fit's last point (ticks, samples since the first)
    bool last_late_ = false;           // and it came over 1.5 ticks' time after the one before
    uint32_t fit_ticks_ = 0; // the clock's tick count at the last point
};

} // namespace chompi
