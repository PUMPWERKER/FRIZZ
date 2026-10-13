/** @file FxShifter.h
 *  @brief The shifter, a two-tap delay-line pitch shifter tuned in semitones.
 *
 *  The swoop, the feedback and the stereo offset are after Bastl Instruments' Kastle 2 FX
 *  Wizard shifter (github.com/bastl-instruments/kastle2, code/src/apps/FxWizard/).
 *  Copyright (c) 2024 Marek Mach, Vaclav Mach (Bastl Instruments), MIT License: see
 *  LICENSE-kastle2.
 */
#pragma once
#include "FxCommon.h"

namespace chompi
{

/** A pitch shifter in semitones. Two taps read the input at 2^(semitones/12) its speed,
 *  each living for a 30ms stretch of delay before it wraps and starts over, half a life
 *  apart and crossfaded with triangles that always sum to one.
 *
 *  Where a tap starts over is searched for: within +-7.5ms of its nominal start, the place
 *  whose next samples best match what the other tap is about to play (every 6th offset,
 *  then every offset around the best, comparing 16 samples spread over 128). Without that, the two
 *  taps cross in an arbitrary phase relation, which pulls the pitch off by up to most of a
 *  semitone and makes the level wobble. (Kastle's shifter has one tap that fades out and in
 *  at every wrap, which is what made it buzz.) The search is the shifter's costliest moment,
 *  so it's spread over two audio blocks (#137): it starts when a tap falls silent, which
 *  then stays silent at the crossing for a block, the other alone, and the rest of the
 *  search places it as far on as the other tap has moved since.
 *
 *  Swoop is Kastle's trigger envelope (0.1s up, 1s down), fired by the key press, pushing
 *  the shift up to 2 octaves further in its direction and back. Feedback recirculates the
 *  shifted output, so each pass shifts again and the shift spirals.
 *  Params: 0 shift (kNumShifts steps, -12 to +12 semitones, the centre dry), 1 feedback,
 *  2 swoop, 3 grain: the taps' window, 10ms (glitchy) through 30ms (the centre) to 50ms
 *  (smooth); page 2: 5 stereo (the right channel up to a semitone higher). Page 2's Mix
 *  (FxOutput.h) puts the dry signal under the shifted one, a harmony. */
class Shifter : public FxBase
{
public:
    enum Param
    {
        SHIFT,
        FEEDBACK,
        SWOOP,
        GRAIN,
        STEREO = 5,
    };

    static const size_t kNumShifts = 25; // -12..+12 semitones

    FX_ONCE void Init(float sample_rate)
    {
        ring_.Clear();
        for (size_t c = 0; c < 2; c++)
        {
            window_[c] = 0.f;
            delay_[c][0] = kGuard + static_cast<float>(kSearch);
            delay_[c][1] = kGuard + static_cast<float>(kSearch) + .5f * kWindowFrames;
        }
        grain_frames_ = kWindowFrames;
        for (size_t c = 0; c < 2; c++)
        {
            window_frames_[c] = kWindowFrames;
            waiting_[c] = held_[c] = 0;
        }
        still_[0] = still_[1] = true;
        apart_ = false;
        fade_ = 0.f;
        fade_step_ = 1.f / kWindowFrames;
        gate_.Init();
        env_.Reset();
        env_attack_inc_ = 1.f / (.1f * sample_rate);
        env_decay_coeff_ = Decay60dBCoeff(1.f, sample_rate);

        SetParam(SHIFT, .5f);
        for (size_t i = 1; i < kNumFxParams; i++)
            SetParam(i, 0.f);
        SetParam(GRAIN, .5f); // the centre, 30ms
        SnapParams();
    }

    FRIZZ_HOT void Process(float* l, float* r)
    {
        const float gate = gate_.Process();

        if (gate_.TakePress())
            env_.Press();
        env_.Process(env_attack_inc_, env_decay_coeff_);

        // off: only the buffer goes on, so a punch-in has the recent sound to shift; the
        // knobs where they're going
        if (gate_.Asleep())
        {
            for (size_t c = 0; c < 2; c++)
            {
                if (waiting_[c])
                    FinishSplice(c);
                window_frames_[c] = grain_frames_; // no tap sounds: the grain at once
            }
            SnapParams();
            ring_.WriteFrame(SoftClip(*l), SoftClip(*r));
            still_[0] = still_[1] = true;
            return;
        }
        const float dry = dry_.Process();
        const float feedback = feedback_.Process();

        // the speed per channel; recomputed every kSwoopUpdate samples while swooping (2
        // powf: every sample was ~6% of the CPU for the swoop's 1.3s), and once when it ends;
        // otherwise when a knob changed it (SetParam), so only the audio callback writes it
        const float swoop = env_.value * swoop_;
        const bool swoop_on = swoop > .0001f;
        const float dir = semitones_ > 0 ? 1.f : (semitones_ < 0 ? -1.f : 0.f);
        if ((swoop_on && swoop_tick_++ % kSwoopUpdate == 0) || (swooping_ && !swoop_on))
        {
            ratios_changed_ = false;
            UpdateRatios(semitones_ + dir * swoop * kSwoopSemitones);
            swooping_ = swoop_on;
        }
        else if (ratios_changed_)
        {
            // a knob between two swoop updates keeps the swoop
            ratios_changed_ = false;
            UpdateRatios(swooping_ ? semitones_ + dir * swoop * kSwoopSemitones
                                   : static_cast<float>(semitones_));
        }
        if (__builtin_expect((still_[0] && ratio_[0] != 1.f) || (still_[1] && ratio_[1] != 1.f), 0))
            Move(gate * (1.f - dry));
        // back at stereo 0 after an offset, while the right channel fades to the left's taps
        // (Relock, #226): the channels with the fade, out of line, so the loop here stays as
        // it was
        if (__builtin_expect(apart_, 0) && (fade_ > 0.f || ratio_[0] == ratio_[1]) && Relock())
            FadeChannels(l, r, gate, dry, feedback);
        else
            Channels<false>(l, r, gate, dry, feedback);
    }

    /** The taps of both channels on by a sample, read, written back with the feedback, and
     *  the output; with kFade, the right channel's output faded towards the left's taps by
     *  fade_ (Relock) */
    template <bool kFade>
    __attribute__((always_inline)) inline void Channels(float* l, float* r, float gate, float dry,
                                                        float feedback)
    {
        float* const io[2] = {l, r};
        for (size_t c = 0; c < 2; c++)
        {
            const float ratio = ratio_[c];
            const bool up = ratio > 1.f;
            float* const d = delay_[c];

            // both taps' delays move by 1 - ratio a sample; the window moves through their
            // lives at the same pace, so each tap covers the grain's window of delay per life
            const float window = window_frames_[c];
            const float max_delay = window + kDelayPast;
            d[0] = fclamp(d[0] + 1.f - ratio, 1.f, max_delay);
            d[1] = fclamp(d[1] + 1.f - ratio, 1.f, max_delay);
            const float prev = window_[c];
            float w = prev;
            if (waiting_[c])
            {
                // a tap waits, silent, for the rest of its search, a block after its start
                if (++held_[c] >= kSpliceHold)
                    FinishSplice(c);
            }
            else
            {
                w = prev + fabsf(1.f - ratio) / window;
                if (w >= 1.f)
                {
                    w = 0.f; // tap 0 is silent here
                    StartSplice(c, 0, ratio, up);
                }
                else if (prev < .5f && w >= .5f)
                {
                    w = .5f; // tap 1 is silent here
                    StartSplice(c, 1, ratio, up);
                }
            }
            window_[c] = w;

            // triangles: tap 0 peaks mid-window and is silent at the wrap, tap 1 the reverse
            const float w0 = 1.f - fabsf(2.f * w - 1.f);
            const float a = ring_.Read(c, d[0]);
            const float b = ring_.Read(c, d[1]);
            float wet = a * w0 + b * (1.f - w0);
            if (kFade && c == 1)
                wet += fade_ * (LeftTaps() - wet);

            ring_.Write(c, SoftClip(*io[c] + wet * feedback));

            const float out = wet + dry * (*io[c] - wet);
            *io[c] += gate * (out - *io[c]);
        }
        ring_.Advance();
    }

    /** Channels with the right channel's fade (Relock) */
    __attribute__((noinline)) void FadeChannels(float* l, float* r, float gate, float dry,
                                                float feedback)
    {
        Channels<true>(l, r, gate, dry, feedback);
    }

    /** The slewed parameters jump to their targets, at Init */
    void SnapParams()
    {
        dry_.Snap();
        feedback_.Snap();
    }

    void SetParam(size_t param, float val) override
    {
        switch (param)
        {
        case SHIFT:
            semitones_ = static_cast<int>(StepIndex(val, kNumShifts)) - (kNumShifts - 1) / 2;
            dry_.target = semitones_ == 0 ? 1.f : 0.f;
            break;
        case SWOOP:
            swoop_ = val;
            break;
        case FEEDBACK:
            feedback_.target = val * .6f;
            break;
        case STEREO:
            stereo_semitones_ = val;
            break;
        case GRAIN:
            // 30ms in the middle, 10ms to 50ms at the ends; picked up at the next splices
            // (NextWindow)
            grain_frames_ = val < .5f ? kWindowFrames * (1.f / 3.f + 4.f / 3.f * val)
                                      : kWindowFrames * (1.f + 4.f / 3.f * (val - .5f));
            return; // the speeds don't change
        default:
            break;
        }
        ratios_changed_ = true;
    }

private:
    static const size_t kBufSize = 4096;
    static const size_t kBufMask = kBufSize - 1;
    static constexpr float kWindowFrames = 1440.f; // a tap's life, 30ms of delay
    static const int kSearch = 360;                // +-7.5ms: aligns notes down to 67Hz
    static const int kCoarseStep = 6;              // the search's first pass, in samples,
    static const int kFineRange = 5;               // then every offset this close to the best
    static const size_t kCorrPoints = 16;          // compared over 128 samples,
    static const size_t kCorrStep = 8;             // every 8th
    // the closest a tap starts to the write head, so the comparison's reads exist yet: the
    // candidates read up to 120 samples on, times the speed, which is below 1 going down
    static constexpr float kGuard = 130.f;
    static constexpr float kMinWindowFrames = kWindowFrames / 3.f;      // grain at the bottom
    static constexpr float kMaxWindowFrames = kWindowFrames * 5.f / 3.f; // and at the top
    static const uint8_t kSpliceHold = 24; // samples a tap waits for its splice: a block
    // a tap lives through two waits for a splice, at its start and halfway, moving on by
    // 1 - ratio a sample through them: going up, it starts that much further back
    static constexpr float kMaxRatio = 8.5f; // 37 semitones: shift, swoop and stereo at the top
    static constexpr float kHoldsPast = 2.f * kSpliceHold * (kMaxRatio - 1.f);
    // a tap's longest delay is the grain's window plus this; the buffer holds the longest
    static constexpr float kDelayPast = kGuard + 2.f * kSearch + kFineRange + 2.f + kHoldsPast;
    static_assert(kMaxWindowFrames + kDelayPast < kBufSize, "the longest grain fits the buffer");
    static constexpr float kSwoopSemitones = 24.f; // how far the swoop pushes at the top
    // the closest a tap that goes on with a new window comes to the write head (NextWindow):
    // a few samples, for a swoop that speeds up through the wait
    static constexpr float kTapFloor = 8.f;

    /** A channel's taps stand still while the shifter is off and while the channel runs at
     *  its own speed (ratio 1); they move again with the speed they move at now. Where a tap
     *  would then run into the end of its delay before its life is over, they were left for
     *  the other direction (by Init, which places them for going down, or by the shift turned
     *  the other way meanwhile, #140), or, going up, a tap is beyond a grain turned shorter
     *  meanwhile (#167), so the taps start afresh: tap 1 halfway through its life, carrying
     *  the sound, tap 0 silent at the start of its own, spliced to match it as at any
     *  crossing. Otherwise they go on from where they were.
     *
     *  At stereo 0, where nothing of the taps is heard yet (wet, the share of them in the
     *  output: punched in, the gate's first step; or the shift leaving the centre, where the
     *  output was dry), the right channel takes the left's taps as they move on, so the two
     *  are the same again where a stereo offset had set them apart, and stay so (StartSplice,
     *  #223). While it plays, Relock does it (#226) */
    __attribute__((noinline)) void Move(float wet)
    {
        for (size_t c = 0; c < 2; c++)
        {
            const float ratio = ratio_[c];
            if (!still_[c] || ratio == 1.f)
                continue;
            still_[c] = false;
            if (c == 1 && wet <= kFxGateCoeff && !still_[0] && ratio_[0] == ratio)
            {
                TakeLeft();
                continue;
            }
            const bool up = ratio > 1.f;
            float* const d = delay_[c];
            const float w = window_[c];
            const float window = window_frames_[c];
            // what's left of each tap's life, in delay (tap 0's ends at w 1, tap 1's at .5),
            // and its waits for a splice
            const float left[2] = {(1.f - w) * window, (w < .5f ? .5f - w : 1.5f - w) * window};
            const float holds = 2.f * kSpliceHold * fabsf(1.f - ratio);
            const float max_delay = window + kDelayPast;
            bool runs_out = false;
            for (size_t t = 0; t < 2; t++)
                runs_out |= up ? d[t] - left[t] - holds < 1.f || d[t] > max_delay
                               : d[t] + left[t] + holds > max_delay;
            if (!runs_out)
                continue;
            window_[c] = 0.f;
            d[1] = (up ? window + holds : 0.f) + kGuard + kSearch + (up ? -.5f : .5f) * window;
            StartSplice(c, 0, ratio, up);
        }
    }

    /** The right channel takes the left's taps as they stand, its life, its splice: from here
     *  on the two are the same (Move, Relock) */
    void TakeLeft()
    {
        window_[1] = window_[0];
        window_frames_[1] = window_frames_[0];
        delay_[1][0] = delay_[0][0];
        delay_[1][1] = delay_[0][1];
        nominal_[1] = nominal_[0];
        other_at_[1] = other_at_[0];
        waiting_[1] = waiting_[0];
        held_[1] = held_[0];
        follows_ = waiting_[0] != 0;
        apart_ = false;
        fade_ = 0.f;
    }

    /** Back at stereo 0 while the shifter plays, the two channels' taps are where the offset
     *  left them, at other points of their lives, and stay so (#226): moved to the left's, the
     *  right's taps would jump, as only a silent one can move unheard. So the right channel's
     *  output fades over a life from its own taps to the left's taps read from its own input
     *  (LeftTaps), the triangles as the taps' own, and at the end takes them (TakeLeft): it
     *  reads there already, so nothing jumps. Through the fade both run on as before, each
     *  splicing on its own. Turned apart again meanwhile, it fades back to its own. Out of
     *  line, as the channels with the fade (FadeChannels): Process checks apart_, and once
     *  the speeds are the same again or the fade runs, calls it. True while the fade runs */
    __attribute__((noinline)) bool Relock()
    {
        if (ratio_[0] == ratio_[1] && !still_[0] && !still_[1])
        {
            if (fade_ == 0.f)
                fade_step_ = 1.f / window_frames_[0];
            fade_ += fade_step_;
            if (fade_ >= 1.f)
                TakeLeft();
        }
        else
            fade_ = fmaxf(0.f, fade_ - fade_step_);
        return fade_ > 0.f;
    }

    /** The left channel's two taps, as they stand this sample, read from the right's input */
    float LeftTaps()
    {
        const float w0 = 1.f - fabsf(2.f * window_[0] - 1.f);
        return ring_.Read(1, delay_[0][0]) * w0 + ring_.Read(1, delay_[0][1]) * (1.f - w0);
    }

    /** Where tap t, falling silent, starts over: its nominal start (the far end of the
     *  window going up, the near end going down) moved by up to kSearch to where its next
     *  samples best match the other tap's, by normalised correlation (SpliceSearch). The
     *  first kFirstGroups of the search's groups now, the rest in FinishSplice. Once per
     *  window, so out of line: inlined twice, the search made Process three times the size.
     *
     *  At stereo 0 the two channels' taps are the same, from Init on, as long as nothing
     *  turns them apart: there the right channel takes the left's search, begun this sample
     *  for the same taps, instead of its own (#223). Searched on their own, a hair of
     *  difference between the inputs (the device's analog paths) picked other lags, and from
     *  there on the channels read from other places: summed to mono, a moving comb filter */
    __attribute__((noinline)) void StartSplice(size_t c, int t, float ratio, bool up)
    {
        const float other = delay_[c][1 - t];
        window_frames_[c] = NextWindow(other, ratio, up);
        nominal_[c] = (up ? window_frames_[c] + 2.f * kSpliceHold * (ratio - 1.f) : 0.f) + kGuard
                      + kSearch;
        follows_ = c == 1 && waiting_[0] == 1 + t && held_[0] == 0 && ratio_[0] == ratio
                   && other_at_[0] == other && nominal_[0] == nominal_[1];
        if (!follows_)
        {
            search_[c].Begin(ring_.buf[c], ring_.Last(), nominal_[c], other, ratio);
            search_[c].Run(kFirstGroups);
        }
        other_at_[c] = other;
        waiting_[c] = static_cast<uint8_t>(1 + t);
        held_[c] = 0;
    }

    /** The window for the life a tap starts now, at a splice: the grain, as far as the other
     *  tap fits it. That one is halfway through its life, placed for the window so far, and
     *  lives the rest of it at the new one, after this splice's wait. Its delay has to stay
     *  within the new window's reach (Process's clamp): going up it shrinks, by half the new
     *  window and the wait, from where it is, which a shorter window may already have left
     *  behind; going down it grows by them. Where a turn of the grain would run it into either
     *  end, where it plays unshifted, a click (#167), the window goes as far as it fits, and
     *  the rest of the way at the next splices; the window it had always fits. Not so where the
     *  shift turned the other way while it plays: the other tap was placed for the other
     *  direction and runs into its end anyway, as before #167, and no window fits it: going up,
     *  it came out at or below 0, and the taps stopped moving through their lives, stuck at
     *  delay 1, unshifted (#221); going down, up to the whole buffer, the taps' reach past it.
     *  So the window stays within the grain knob's reach */
    float NextWindow(float other, float ratio, bool up) const
    {
        const float wait = kSpliceHold * fabsf(1.f - ratio);
        // a sample inside, so the clamp doesn't touch it
        const float lowest = up ? other + 1.f - kDelayPast
                                : 2.f * (other + wait + 1.f - kDelayPast);
        const float highest = up ? 2.f * (other - wait - kTapFloor) : kBufSize;
        return fclamp(fminf(fmaxf(grain_frames_, lowest), highest), kMinWindowFrames,
                      kMaxWindowFrames);
    }

    /** The rest of channel c's search, and the waiting tap starts where it found, as far on
     *  as the other tap has moved since: the two in the relation it chose. The search's reads
     *  are all behind where the write head was when it began, so the block written since
     *  changes none of them: it finds what it would have found all at once. The right channel
     *  following the left finishes the same sample, after it, with what the left's found */
    __attribute__((noinline)) void FinishSplice(size_t c)
    {
        const int t = waiting_[c] - 1;
        float* const d = delay_[c];
        const bool follows = c == 1 && follows_;
        if (!follows)
            search_[c].Run(SpliceSearch::kGroups);
        const int lag = search_[follows ? 0 : c].best.lag;
        d[t] = nominal_[c] + static_cast<float>(lag) + (d[1 - t] - other_at_[c]);
        waiting_[c] = 0;
    }

public:
    /** A splice search: the lag from a tap's nominal start, in buffer b, whose next samples
     *  best match those of the other tap. Every kCoarseStep-th lag, then every lag around the
     *  best, in groups of kGroup lags, which Run can take a few at a time.
     *
     *  It's what the peak block costs, so it's made cheap without changing a score: a group's
     *  sums are separate chains, each still added up point by point in the same order; the
     *  reads go off a pointer where the search's reach doesn't wrap the ring; and a lag's
     *  sqrtf and division run only where it can beat the best (Best). The splice check
     *  (test/splice.cpp) holds it against the search as it was (#137) */
    struct SpliceSearch
    {
        /** What the search compares: k samples on, both taps will have moved k * ratio
         *  through the input: the other tap's samples (at delay other), and where a
         *  candidate at lag 0 reads (the nominal start), once for every lag */
        void Begin(const float* buf, size_t last, float nominal, float other, float ratio)
        {
            b = buf;
            for (size_t k = 0; k < kCorrPoints; k++)
            {
                const float ahead = static_cast<float>(k * kCorrStep) * ratio;
                ref[k] = b[(last - static_cast<size_t>(fmaxf(other - ahead, 1.f))) & kBufMask];
                start[k] = last - static_cast<size_t>(nominal - ahead);
            }
            wraps = false;
            for (size_t k = 0; k < kCorrPoints; k++)
            {
                wraps |= ((start[k] - kMaxLag) & kBufMask) + (kMaxLag - kMinLag) > kBufMask;
                at[k] = b + (start[k] & kBufMask);
            }
            best = Best();
            group = 0;
        }

        /** Scores up to n more groups; true once every lag is scored, the best in best.lag */
        __attribute__((noinline)) bool Run(int n)
        {
            return wraps ? Run<true>(n) : Run<false>(n);
        }

        /** The best lag so far and its score, dot / sqrt(energy + 1e-9) */
        struct Best
        {
            int lag = 0;
            float score = -1e30f;
            // dot / sqrtf(e) > score > 0 needs dot > 0 and dot^2 > score^2 * e: a lag clearly
            // short of that can't win, and skips the sqrtf and the division. The margin is
            // far above their rounding (a few parts in 2^24), so no lag that wins is skipped.
            // Only for a score over 1e-6, which keeps the right side clear of underflow;
            // below, below and floor let every lag through
            float below = -__builtin_inff(); // a dot at most this can't win
            float floor = 0.f;               // nor one whose dot^2 is under this times e

            /** A lag that beats the best takes its place; ties keep the earlier one */
            inline void Offer(int l, float dot, float energy)
            {
                const float e = energy + 1e-9f;
                if (dot > below && dot * dot >= floor * e)
                    Score(l, dot, e);
            }

            /** The few lags that may win, out of line: Offer is inlined kGroup times */
            __attribute__((noinline)) void Score(int l, float dot, float e)
            {
                const float v = dot / sqrtf(e);
                if (v > score)
                {
                    score = v;
                    lag = l;
                    const bool skips = score > 1e-6f;
                    below = skips ? 0.f : -__builtin_inff();
                    floor = skips ? kSkipMargin * score * score : 0.f;
                }
            }
        };

        static const int kGroup = 4; // lags scored at once
        static const int kCoarseGroups = (2 * kSearch / kCoarseStep + kGroup) / kGroup;
        static const int kFineGroups = (2 * kFineRange + kGroup) / kGroup;
        static const int kGroups = kCoarseGroups + kFineGroups;
        // the lags it reads: a group past the last lag scores lags beyond it, unoffered
        static const int kMinLag = -kSearch - kFineRange;
        static const int kMaxLag = -kSearch + (kCoarseGroups * kGroup - 1) * kCoarseStep;
        static_assert(kFineGroups * kGroup - kFineRange <= kMaxLag - kSearch,
                      "the fine pass reads within the coarse pass's reach");

        const float* b;
        float ref[kCorrPoints];
        size_t start[kCorrPoints];    // the ring's index (unmasked) of each candidate at lag 0
        const float* at[kCorrPoints]; // the same, a pointer, for reads that don't wrap
        bool wraps;
        Best best;
        int group; // the next group to score
        int coarse;

    private:
        static constexpr float kSkipMargin = .9999f;

        template <bool kWraps>
        bool Run(int n)
        {
            for (; n > 0 && group < kGroups; n--, group++)
            {
                if (group < kCoarseGroups)
                {
                    ScoreGroup<kCoarseStep, kWraps>(
                        -kSearch + group * kGroup * kCoarseStep, kSearch);
                    if (group == kCoarseGroups - 1)
                        coarse = best.lag;
                }
                else
                    ScoreGroup<1, kWraps>(
                        coarse - kFineRange + (group - kCoarseGroups) * kGroup,
                        coarse + kFineRange);
            }
            return group == kGroups;
        }

        /** Scores the kGroup lags lag, lag + kStep, ... and offers those up to last_lag to
         *  best, in that order. A lag adds to the delay, so the candidate reads lag samples
         *  further back; masked only where the search's reads wrap the ring */
        template <int kStep, bool kWraps>
        inline __attribute__((always_inline)) void ScoreGroup(int lag, int last_lag)
        {
            float dot[kGroup] = {}, energy[kGroup] = {};
            for (size_t k = 0; k < kCorrPoints; k++)
            {
                const size_t i = (start[k] - static_cast<size_t>(lag)) & kBufMask;
                const float* p = at[k] - lag;
                // opaque, so the reads go off p with offsets, not a register each (GCC's habit)
                asm("" : "+r"(p));
                const float r = ref[k];
                for (int j = 0; j < kGroup; j++)
                {
                    const float x = kWraps ? b[(i - j * kStep) & kBufMask] : *(p - j * kStep);
                    dot[j] += x * r;
                    energy[j] += x * x;
                }
            }
            for (int j = 0; j < kGroup; j++)
                if (lag + j * kStep <= last_lag)
                    best.Offer(lag + j * kStep, dot[j], energy[j]);
        }
    };

    /** The whole search at once (Splice, the splice check) */
    static int SpliceLag(const float* b, size_t last, float nominal, float other, float ratio)
    {
        SpliceSearch s;
        s.Begin(b, last, nominal, other, ratio);
        s.Run(SpliceSearch::kGroups);
        return s.best.lag;
    }

private:
    // the search's groups at its start, the rest a block later: about half its time each
    static const int kFirstGroups = SpliceSearch::kGroups / 2;
    /** Each channel's speed: 2^(semitones/12), the right channel stereo semitones higher */
    void UpdateRatios(float semitones)
    {
        ratio_[0] = powf(2.f, semitones / 12.f);
        ratio_[1] = powf(2.f, (semitones + stereo_semitones_) / 12.f);
        for (size_t c = 0; c < 2; c++)
            still_[c] |= ratio_[c] == 1.f;
        apart_ |= ratio_[0] != ratio_[1];
    }

    StereoRing<kBufSize> ring_;
    // per channel: the splice search under way, its nominal start, the other tap's delay
    // when it began, the tap waiting for it (1 + tap, 0 none) and how long it has waited
    SpliceSearch search_[2];
    float nominal_[2], other_at_[2];
    uint8_t waiting_[2], held_[2];
    bool follows_ = false; // the right channel's splice takes the left's search (StartSplice)
    // the two channels' taps may differ: a stereo offset moved them apart since they were the
    // same; and the right channel's fade to the left's taps, 0..1, and its step (Relock)
    bool apart_;
    float fade_, fade_step_;
    bool still_[2]; // a channel's taps stood still (off, or at ratio 1) since they last moved
    float window_[2];   // through the taps' lives, 0..1; tap 0 starts over at 0, tap 1 at .5
    float delay_[2][2]; // per channel, per tap, frames behind the last write
    float ratio_[2] = {1.f, 1.f};
    volatile bool ratios_changed_ = false; // set by SetParam, picked up by Process
    int semitones_ = 0;
    float swoop_ = 0.f;
    bool swooping_ = false;
    uint32_t swoop_tick_ = 0;
    static const uint32_t kSwoopUpdate = 8; // samples between the swoop's speed updates
    float stereo_semitones_ = 0.f;
    float grain_frames_ = kWindowFrames; // the grain, from the knob
    float window_frames_[2];             // per channel, the taps' window now (NextWindow)
    Smoothed dry_;
    Smoothed feedback_;
    PressEnvelope env_;
    float env_attack_inc_, env_decay_coeff_;
};

} // namespace chompi
