/** @file SettingsPage.h
 *  @brief The settings page, while the mode switch is up: what is set once rather than played.
 *  The play page (NormalPage.h) owns it, routes the hand's keys here while the switch is up
 *  (ui.h) and draws it instead of itself; the loop, the effects and MIDI play on meanwhile.
 *  MANUAL.md describes it for players.
 *
 *  The keys:
 *
 *    white keys 1-14     MIDI channel 1-14                   light blue, the one in force lit
 *    white key 15        channel 15, again 16, again 15 ...  light blue for 15, white for 16
 *  The dark keys by topic (#97): the lower octave MIDI in, the upper MIDI out and the rest.
 *
 *    C# (lower octave)   all channels                        light blue
 *    D#                  the effect channels (1-15) over the jack too, on / off
 *                                                            light blue, lit when on
 *    F#                  MIDI transport following, on / off  green, lit when on
 *    G#                  the tempo factor, each press the next: x1, x2, x1/2, on a clock,
 *                        a loop or a tapped tempo alike (TempoClock.h)
 *                                                            yellow, red, light blue,
 *                        lit for the first half of every beat the effects follow, else dim
 *    A#                  the clock source, each press the next: Auto, TRS, USB, internal
 *                                                            white, orange, blue, pink
 *    C# (upper octave)   MIDI out, each press the next: off, the jack, the jack and USB
 *                                                            dim, orange, white
 *    D# (upper octave)   MIDI feedback, on / off             light blue, lit when on
 *  The upper octave's F#, G# and A# are the input (#177): the source, then whether you hear
 *  it, then how (#243):
 *
 *    F# (upper octave)   the source, each press the next: AUX, the mic, USB (a computer's
 *                        audio, #196)                        green, magenta, blue, lit; USB
 *                                                            dim while USB audio is off
 *    SHIFT + F#          with AUX or the mic: AUX mono, on / off; with USB: USB audio, on /
 *                        off (FRIZZ a USB sound card or not, UsbDevice.h: it leaves the bus
 *                        and comes back, MIDI gone for a moment; refused, 3 red blinks, while
 *                        the card is lent)                   white while SHIFT is held, lit
 *                                                            when on
 *    G# (upper octave)   the source live, on the outputs, on / off
 *                                                            white, lit when on
 *    A# (upper octave)   the source's effects: through them / dry (past them and the
 *                        compressor)                         green / white, lit; dark while
 *                                                            the source isn't live
 *
 *  AUX's jack picks AUX or the mic at power-on and whenever a cable goes in or out
 *  (chompi_main.cpp), but not while USB is the source: it stays. F# picks another until then.
 *  The source itself isn't saved, USB audio is. G# and A# show and set the source's own, each
 *  source's saved (passthroughEngine.h keeps all three, so the jack brings up the other's), as
 *  is its input gain: VOLUME's page 2, here and on the play page, sets the one heard. Out of
 *  the box AUX and USB are live through the effects, the mic through them but not live (a
 *  speaker would feed it back).
 *
 *    knob 1              the LEDs' brightness (#176), 25-100 % by 5 % a detent; SHIFT + turn
 *                        the next quarter, 25, 50, 75, 100 %; SHIFT + press 100 %. Its LED is
 *                        white, dimmed as all are, so it shows the level. Knobs 2-4 do
 *                        nothing here, their LEDs dark
 *
 *    VOLUME              turned: page 1 the output, page 2 the input gain, as on the play
 *                        page (NormalPage.h, which handles it); a press, page 1 or 2; no
 *                        page 3, and SHIFT + turn does nothing here
 *    SHIFT + VOLUME press, both held 2 s     a bug report (EventLog.h), once per hold;
 *                                            the transport LEDs blink as on the play page
 *    SHIFT + LOOP, both held 2 s             the card lent to the computer as a USB drive,
 *                                            or taken back (CardLend.h), once per hold;
 *                                            LOOP blue while it's lent, 3 red blinks refused
 *
 *  The CHOMPI key is lit white while SHIFT is held, as on the play page. Each key acts on its
 *  press. A setting the card can't take blinks the key changed last red 3 times (LedSignal.h),
 *  again with each retry. A key that went down on the play page stays the play page's until
 *  it's let go, and the other way round (ui.h), so flipping the switch with a key held doesn't
 *  let go of it. A scene mode (SAVE, COPY, DELETE) is left as the switch goes up.
 *  VOLUME's LED shows the battery's level (Hardware's
 *  BatteryLevel: white while the charging cable is in, green above 3.3 V, yellow below, red
 *  below 3 V), but for 2 s after VOLUME was turned or pressed, when it shows its page as the
 *  play page does; the transport LEDs are purple, so the page is never taken for the play page.
 *
 *  Each setting goes to the card as the master settings do (MasterSettings.h).
 */
#pragma once
#include "hardware.h"
#include "LedColors.h"
#include "LedSignal.h"
#include "CardLend.h"
#include "MasterSettings.h"
#include "MidiClock.h"
#include "MidiControl.h"
#include "PanelKeys.h"
#include "PanelLeds.h"
#include "passthroughEngine.h"

// the play page's logic, a key or a frame now and then and never per sample: built for size,
// as FRIZZ's code space is tight (SRAM_EXEC, docs/CAPACITY.md)
#pragma GCC push_options
#pragma GCC optimize("Os")

namespace chompi
{

class SettingsPage
{
public:
    void Init(PassthroughEngine* engine, MidiControl* midi, Hardware* hw,
              const MasterSettings& master)
    {
        engine_ = engine;
        midi_ = midi;
        hw_ = hw;
        mono_ = master.mono;
        engine_->SetMonoInput(mono_);
        factor_ = master.clock_factor == 50    ? ClockFactor::HALF
                  : master.clock_factor == 200 ? ClockFactor::DOUBLE
                                               : ClockFactor::ONE;
        midi_->SetClockFactor(factor_);
        midi_->SetClockSource(static_cast<ClockSource>(master.clock_source));
        midi_->SetMidiOut(static_cast<MidiOutPorts>(master.midi_out));
        midi_->SetBlockTrs(master.midi_block_trs);
        midi_->SetFeedback(master.midi_feedback);
        // on the knob's steps: a value set over SysEx between them goes to the nearest
        percent_ = Brightness((master.led_brightness + kBrightnessStep / 2) / kBrightnessStep
                              * kBrightnessStep);
        SetLedPercent(percent_);
        saved_dry_[kAux] = master.aux_dry;
        saved_dry_[kMic] = master.mic_dry;
        saved_dry_[kUsb] = master.usb_dry;
        saved_live_[kAux] = master.aux_live;
        saved_live_[kMic] = master.mic_live;
        saved_live_[kUsb] = master.usb_live;
        for (size_t src = 0; src < kNumSources; src++)
        {
            engine_->SetDry(src, saved_dry_[src]);
            engine_->SetLive(src, saved_live_[src]);
        }
        SetGain(kAux, master.gain_aux / 100.f);
        SetGain(kMic, master.gain_mic / 100.f);
        SetGain(kUsb, master.gain_usb / 100.f);
        for (size_t src = 0; src < kNumSources; src++)
            saved_gain_[src] = gain_[src];
        usb_audio_ = master.usb_audio;
    }

    /** A key going down on the page, shift: SHIFT held from the play page. True if it changed
     *  a setting, which then goes to the card */
    bool Key(int key, bool shift, uint32_t now)
    {
        shift = shift || ShiftHeld();
        // the source heard, read once: the jack may switch it meanwhile
        const size_t src = engine_->Source();
        if (key == static_cast<int>(kInSourceKey) && !shift)
        {
            // AUX, the mic, USB, AUX ...: until the jack picks again, not for the card
            engine_->SetSource((src + 1) % kNumSources);
            return false;
        }
        if (key == static_cast<int>(kInFxKey) || key == static_cast<int>(kLiveKey))
        {
            // the source's own, for the card; A# dark (not live) does nothing, and blinks red
            const bool fx = key == static_cast<int>(kInFxKey);
            if (fx && !engine_->Live(src))
            {
                dark_fx_.Start(now);
                return false;
            }
            if (fx)
                engine_->SetDry(src, !engine_->Dry(src));
            else
                engine_->SetLive(src, !engine_->Live(src));
            bool& saved = fx ? saved_dry_[src] : saved_live_[src];
            const bool now = fx ? engine_->Dry(src) : engine_->Live(src);
            if (now == saved)
                return false;
            saved = now;
        }
        else if (key == static_cast<int>(kBrightnessKnobKey))
        {
            // SHIFT + press: full, as every knob's reset
            if (!shift || !SetBrightness(100))
                return false;
        }
        else if (key == static_cast<int>(kInSourceKey) && src == kUsb)
        {
            // USB audio, which leaves the bus and comes back: not with the card lent there
            if (cardlend::Get().lent)
            {
                no_usb_switch_.Start(now);
                return false;
            }
            usb_audio_ = !usb_audio_;
        }
        else if (key == static_cast<int>(kInSourceKey))
        {
            if (!SetMono(!mono_))
                return false;
        }
        else if (!Apply(key))
            return false;
        unsaved_key_ = key;
        return true;
    }

    /** Knob 1 turned detents on the page (the others do nothing here), shift: SHIFT held from
     *  the play page. True if the brightness changed, which then goes to the card */
    bool KnobTurned(int16_t detents, bool shift)
    {
        int percent = percent_;
        if (shift || ShiftHeld())
        {
            // the next quarter that way, a detent at a time
            for (; detents > 0; detents--)
                percent = (percent / kQuarter + 1) * kQuarter;
            for (; detents < 0; detents++)
                percent = (percent + kQuarter - 1) / kQuarter * kQuarter - kQuarter;
        }
        else
            percent += detents * kBrightnessStep;
        if (!SetBrightness(Brightness(percent)))
            return false;
        unsaved_key_ = kBrightnessKnobKey;
        return true;
    }

    /** The settings' write went through: no key waits for it any more */
    inline void Saved() { unsaved_key_ = kNoKey; }
    /** It failed: the key changed last blinks red, if one is waiting for it */
    void SaveFailed(uint32_t now)
    {
        if (unsaved_key_ != kNoKey)
            refused_.Start(now);
    }

private:
    /** Key's setting: true if it changed */
    bool Apply(int key)
    {
        using S = Hardware::SwId;
        for (uint8_t ch = 0; ch < kNumChannelKeys; ch++)
        {
            if (key == static_cast<int>(kWhiteKeys[ch]))
                return SetChannel(ch + 1);
        }
        if (key == static_cast<int>(kChannel15Key))
            return SetChannel(midi_->Channel() == 15 ? 16 : 15); // 15 first, then 16, 15 ...
        if (key == static_cast<int>(S::KEY_16))
            return SetChannel(0);
        if (key == static_cast<int>(kTransportKey))
        {
            midi_->SetTransport(!midi_->Transport());
            return true;
        }
        if (key == static_cast<int>(kFactorKey))
        {
            // x1, x2, x1/2, x1 ...: ClockFactor's next, round
            factor_ = static_cast<ClockFactor>((static_cast<uint8_t>(factor_) + 1) % 3);
            midi_->SetClockFactor(factor_);
            return true;
        }
        if (key == static_cast<int>(kSourceKey))
        {
            // Auto, TRS, USB, internal, Auto ...
            const uint8_t next = (static_cast<uint8_t>(midi_->GetClockSource()) + 1) % kNumClockSources;
            midi_->SetClockSource(static_cast<ClockSource>(next));
            return true;
        }
        if (key == static_cast<int>(kBlockTrsKey))
        {
            midi_->SetBlockTrs(!midi_->BlockTrs());
            return true;
        }
        if (key == static_cast<int>(kFeedbackKey))
        {
            midi_->SetFeedback(!midi_->Feedback());
            return true;
        }
        if (key == static_cast<int>(kMidiOutKey))
        {
            // off, the jack, the jack and USB, off ...
            const uint8_t next = (static_cast<uint8_t>(midi_->GetMidiOut()) + 1) % kNumMidiOutPorts;
            midi_->SetMidiOut(static_cast<MidiOutPorts>(next));
            return true;
        }
        return false;
    }

    /** The LEDs' brightness, in percent: true if it changed */
    bool SetBrightness(int percent)
    {
        if (percent == percent_)
            return false;
        percent_ = static_cast<uint8_t>(percent);
        SetLedPercent(percent_);
        return true;
    }
    static inline int Brightness(int percent)
    {
        return percent < kMinBrightness ? kMinBrightness : percent > 100 ? 100 : percent;
    }

public:
    /** A key going down or up on the page: SHIFT (CHOMPI), VOLUME's press and LOOP are
     *  watched for the bug report's hold and the card's */
    void Held(int key, bool down, uint32_t now)
    {
        const uint8_t bit = key == static_cast<int>(Hardware::SwId::KEY_26)     ? kShiftHeld
                            : key == static_cast<int>(Hardware::SwId::ENC_6_SW) ? kVolumeHeld
                            : key == static_cast<int>(Hardware::SwId::KEY_28)   ? kLoopHeld
                            : key == kPlayShift                                 ? kPlayShiftHeld
                                                                                : 0;
        if (!bit)
            return;
        const uint8_t was = Holding();
        held_ = down ? held_ | bit : held_ & ~bit;
        const uint8_t holding = Holding();
        if (holding == kBothHeld && was != kBothHeld)
            held_since_ = now;
        else if (holding != kBothHeld)
            reported_ = false;
        if (holding == kCardHeld && was != kCardHeld)
            card_since_ = now;
        else if (holding != kCardHeld)
            card_asked_ = false;
    }
    /** SHIFT held from the play page, from Draw: it counts for the holds too, as for Key */
    inline void PlayShift(bool shift, uint32_t now) { Held(kPlayShift, shift, now); }

    /** True once per hold, when SHIFT and LOOP have both been held kBugReportHoldMs: the card
     *  lent or taken back (CardLend.h) */
    bool CardHeld(uint32_t now)
    {
        if (Holding() != kCardHeld || card_asked_ || now - card_since_ < kBugReportHoldMs)
            return false;
        card_asked_ = true;
        return true;
    }

    /** True once per hold, when SHIFT and VOLUME's press have both been held kBugReportHoldMs */
    bool BugReportHeld(uint32_t now)
    {
        if (Holding() != kBothHeld || reported_ || now - held_since_ < kBugReportHoldMs)
            return false;
        reported_ = true;
        return true;
    }

    /** MIDI's input settings (#177), in RAM only, as all MIDI's (#97): the source (CC 84,
     *  kAux, kMic or kUsb), and the source's effects dry (CC 116) and live (CC 117). The
     *  source's are set even while it isn't live */
    inline void SetSource(size_t source) { engine_->SetSource(source); }
    inline void SetDry(bool dry) { engine_->SetDry(engine_->Source(), dry); }
    inline void SetLive(bool live) { engine_->SetLive(engine_->Source(), live); }

    /** The input gain of the source heard now, 0..1, and setting it: VOLUME's page 2, which
     *  keeps it for the card; CC 86 doesn't (keep false, #97) */
    inline float InGain() const { return gain_[engine_->Source()]; }
    inline void SetInGain(float gain, bool keep)
    {
        const size_t src = engine_->Source();
        SetGain(src, gain);
        if (keep)
            saved_gain_[src] = gain_[src];
    }

    /** Whether FRIZZ should be a USB sound card (SHIFT + F# with USB, the card's usb_audio):
     *  MainLoop switches UsbDevice.h to it (chompi_main.cpp) */
    inline bool UsbAudio() const { return usb_audio_; }

    /** SHIFT (the CHOMPI key) held on this page */
    inline bool ShiftHeld() const { return held_ & kShiftHeld; }

    /** The mono input, from its key. True if it changed */
    bool SetMono(bool mono)
    {
        if (mono == mono_)
            return false;
        mono_ = mono;
        engine_->SetMonoInput(mono_);
        return true;
    }
    inline bool Mono() const { return mono_; }

    /** One of the card's settings (masterfile::kLines' index) to v, as its key would set
     *  it (kCmdDev's SET). False if there's no such setting or it doesn't take v */
    bool Set(size_t line, long v)
    {
        using namespace masterfile;
        if (line >= sizeof(kLines) / sizeof(kLines[0]) || !kLines[line].Takes(v))
            return false;
        // USB audio leaves the bus: not with the card lent there, as its key (Key)
        if (kLines[line].offset == offsetof(MasterSettings, usb_audio) && cardlend::Get().lent)
            return false;
        MasterSettings master;
        Store(master);
        *Field(master, kLines[line]) = static_cast<uint8_t>(kLines[line].kind == Line::SWITCH ? v != 0 : v);
        // what MIDI set without the card (CC 86, 116, 117) stays unless this sets it
        float gain[kNumSources];
        bool dry[kNumSources], live[kNumSources];
        for (size_t src = 0; src < kNumSources; src++)
        {
            gain[src] = gain_[src];
            dry[src] = engine_->Dry(src);
            live[src] = engine_->Live(src);
        }
        // a setter leaves a setting that stays as it is (the clock keeps its state)
        Init(engine_, midi_, hw_, master);
        const size_t set = kLines[line].offset;
        static const uint8_t kGainOf[kNumSources] = {offsetof(MasterSettings, gain_aux),
                                                     offsetof(MasterSettings, gain_mic),
                                                     offsetof(MasterSettings, gain_usb)};
        static const uint8_t kDryOf[kNumSources] = {offsetof(MasterSettings, aux_dry),
                                                    offsetof(MasterSettings, mic_dry),
                                                    offsetof(MasterSettings, usb_dry)};
        static const uint8_t kLiveOf[kNumSources] = {offsetof(MasterSettings, aux_live),
                                                     offsetof(MasterSettings, mic_live),
                                                     offsetof(MasterSettings, usb_live)};
        for (size_t src = 0; src < kNumSources; src++)
        {
            if (set != kGainOf[src])
                SetGain(src, gain[src]);
            if (set != kDryOf[src])
                engine_->SetDry(src, dry[src]);
            if (set != kLiveOf[src])
                engine_->SetLive(src, live[src]);
        }
        midi_->SetChannel(master.midi_channel);
        midi_->SetTransport(master.midi_transport);
        return true;
    }

    /** Its settings into the master settings, for the card */
    void Store(MasterSettings& master) const
    {
        master.mono = mono_;
        master.midi_channel = midi_->Channel();
        master.midi_transport = midi_->Transport();
        master.clock_factor = factor_ == ClockFactor::HALF     ? 50
                              : factor_ == ClockFactor::DOUBLE ? 200
                                                               : 100;
        master.clock_source = static_cast<uint8_t>(midi_->GetClockSource());
        master.midi_out = static_cast<uint8_t>(midi_->GetMidiOut());
        master.midi_block_trs = midi_->BlockTrs();
        master.midi_feedback = midi_->Feedback();
        master.led_brightness = percent_;
        master.aux_dry = saved_dry_[kAux];
        master.mic_dry = saved_dry_[kMic];
        master.usb_dry = saved_dry_[kUsb];
        master.aux_live = saved_live_[kAux];
        master.mic_live = saved_live_[kMic];
        master.usb_live = saved_live_[kUsb];
        master.gain_aux = static_cast<uint8_t>(saved_gain_[kAux] * 100.f + .5f);
        master.gain_mic = static_cast<uint8_t>(saved_gain_[kMic] * 100.f + .5f);
        master.gain_usb = static_cast<uint8_t>(saved_gain_[kUsb] * 100.f + .5f);
        master.usb_audio = usb_audio_;
    }

    /** Every LED; the panel's were cleared. shift: SHIFT held from the play page */
    void Draw(uint32_t now, bool shift)
    {
        SmtLedsOff();

        const uint8_t channel = midi_->Channel();
        for (uint8_t ch = 0; ch < kNumChannelKeys; ch++)
            KeyLed(kWhiteKeys[ch], med_blue, channel == ch + 1);
        KeyLed(kChannel15Key, channel == 16 ? white : med_blue, channel >= 15);
        KeyLed(Hardware::SwId::KEY_16, med_blue, channel == 0);
        KeyLed(kTransportKey, green, midi_->Transport());
        // the factor's colour, on the beats of the tempo the effects follow (TempoClock.h)
        const bool beat = engine_->FxClockPosition() % kPulsesPerBeat < kPulsesPerBeat / 2;
        KeyLed(kFactorKey, kFactorColors[static_cast<uint8_t>(factor_)], beat);
        KeyLed(kSourceKey, kSourceColors[static_cast<uint8_t>(midi_->GetClockSource())], true);
        const MidiOutPorts out = midi_->GetMidiOut();
        KeyLed(kMidiOutKey, out == MidiOutPorts::TRS ? orange : white, out != MidiOutPorts::OFF);
        KeyLed(kBlockTrsKey, med_blue, midi_->BlockTrs());
        KeyLed(kFeedbackKey, med_blue, midi_->Feedback());
        // the input: the source (with SHIFT AUX's mono, or with USB USB audio), live and its
        // effects (dark unless it's live)
        const size_t src = engine_->Source();
        const bool live = engine_->Live(src);
        if (no_usb_switch_.Active(now))
            SmtLed(KeyLedOf(kInSourceKey), red, no_usb_switch_.BlinkLit(now) ? 1.f : 0.f);
        else if (shift || ShiftHeld())
            KeyLed(kInSourceKey, white, src == kUsb ? usb_audio_ : mono_);
        else
            KeyLed(kInSourceKey, kInSourceColors[src], src != kUsb || usb_audio_);
        KeyLed(kLiveKey, white, live);
        if (dark_fx_.Active(now))
            SmtLed(KeyLedOf(kInFxKey), red, dark_fx_.BlinkLit(now) ? 1.f : 0.f);
        else
            SmtLed(KeyLedOf(kInFxKey), engine_->Dry(src) ? white : green, live ? 1.f : 0.f);
        // knob 1 the brightness, white and dimmed with every LED: it shows itself
        for (uint8_t k = 0; k < 4; k++)
            PthLed(kKnobLeds[k], white, k == 0 ? 1.f : 0.f);

        const unsigned level = hw_->GetBatteryLevel();
        const float* battery = level < 4 ? kBatteryColors[level] : green;
        PthLed(kVolumeLed, battery, 1.f);
        PthLed(kTransportLedRev, purple, 1.f);
        PthLed(kTransportLedFwd, purple, 1.f);
        // LOOP: the card lent (blue), or refused (3 red blinks); else dark (CardLend.h)
        cardlend::State& card = cardlend::Get();
        if (card.refused.Active(now))
            PthLed(kLoopLed, red, card.refused.BlinkLit(now) ? 1.f : 0.f);
        else
            PthLed(kLoopLed, blue, card.lent ? 1.f : 0.f);
        PthLed(kChompiKeyLed, white, shift || (held_ & kShiftHeld) ? 1.f : 0.f);
        if (refused_.Active(now))
        {
            const float lit = refused_.BlinkLit(now) ? 1.f : 0.f;
            if (unsaved_key_ == kBrightnessKnobKey)
                PthLed(kKnobLeds[0], red, lit);
            else
                SmtLed(KeyLedOf(static_cast<Hardware::SwId>(unsaved_key_)), red, lit);
        }
    }

private:
    static const size_t kAux = PassthroughEngine::kAux, kMic = PassthroughEngine::kMic,
                        kUsb = PassthroughEngine::kUsb, kNumSources = PassthroughEngine::kNumSources;

    void SetGain(size_t src, float gain)
    {
        gain_[src] = gain < 0.f ? 0.f : (gain > 1.f ? 1.f : gain);
        engine_->SetInputGain(src, gain_[src]);
    }

    bool SetChannel(uint8_t channel)
    {
        if (channel == midi_->Channel())
            return false;
        midi_->SetChannel(channel);
        return true;
    }

    /** A key's LED in its group's colour: full when it's the setting in force, else dim */
    static void KeyLed(Hardware::SwId key, const float* color, bool on)
    {
        SmtLed(KeyLedOf(key), color, on ? 1.f : kFxOffLevel);
    }

    static const uint8_t kNumChannelKeys = 14; // white keys 1-14; the 15th is 15 and 16
    static const Hardware::SwId kChannel15Key = Hardware::SwId::KEY_15;
    // Held's bits: SHIFT 1, VOLUME 2, LOOP 4, the play page's SHIFT 8 (PlayShift); the bug
    // report's hold, the card's
    static const uint8_t kShiftHeld = 1, kVolumeHeld = 2, kLoopHeld = 4, kPlayShiftHeld = 8;
    static const uint8_t kBothHeld = kShiftHeld | kVolumeHeld, kCardHeld = kShiftHeld | kLoopHeld;
    static const int kNoKey = -1, kPlayShift = -2; // no key; PlayShift's for Held
    /** What's held, either SHIFT as the page's */
    inline uint8_t Holding() const { return (held_ | held_ >> 3) & (kShiftHeld | kVolumeHeld | kLoopHeld); }
    static const uint32_t kBugReportHoldMs = 2000;

    // the dark keys by topic (#97): MIDI in on the lower octave, from all channels (KEY_16)
    // on; the rest on the upper
    static const Hardware::SwId kBlockTrsKey = Hardware::SwId::KEY_17;
    static const Hardware::SwId kTransportKey = Hardware::SwId::KEY_18;
    static const Hardware::SwId kFactorKey = Hardware::SwId::KEY_19;
    static const Hardware::SwId kSourceKey = Hardware::SwId::KEY_20;
    static const Hardware::SwId kMidiOutKey = Hardware::SwId::KEY_21;
    static const Hardware::SwId kFeedbackKey = Hardware::SwId::KEY_22;
    // the input (#177): the source (SHIFT: mono), live, its effects (#243)
    static const Hardware::SwId kInSourceKey = Hardware::SwId::KEY_23;
    static const Hardware::SwId kLiveKey = Hardware::SwId::KEY_24;
    static const Hardware::SwId kInFxKey = Hardware::SwId::KEY_25;
    // knob 1's press, the brightness's (#176), and its LED and the others' (PanelLeds.h)
    static const int kBrightnessKnobKey = static_cast<int>(Hardware::SwId::ENC_4_SW);
    static constexpr uint8_t kKnobLeds[4] = {1, 2, 3, 4};
    static const int kBrightnessStep = 5, kQuarter = 25, kMinBrightness = 25; // percent
    // the factor's hue, by ClockFactor: x1/2 light blue, x1 yellow, x2 red
    static constexpr const float* kFactorColors[3] = {med_blue, yellow, red};
    // the source's, by ClockSource: Auto white, TRS orange, USB blue, internal pink
    static constexpr const float* kSourceColors[kNumClockSources] = {white, orange, blue, pink};
    // the input's source's, AUX green, the mic magenta, USB blue
    static constexpr const float* kInSourceColors[3] = {green, magenta, blue};
    // the battery's level, by Hardware::BatteryLevel
    static constexpr const float* kBatteryColors[4] = {white, green, yellow, red};

    PassthroughEngine* engine_ = nullptr;
    MidiControl* midi_ = nullptr;
    Hardware* hw_ = nullptr;
    bool mono_ = false;
    ClockFactor factor_ = ClockFactor::ONE;
    uint8_t percent_ = 100; // the LEDs' brightness
    float gain_[kNumSources] = {.75f, .75f, .75f}; // the input gain, AUX's, the mic's, USB's
    // what the card gets of each source's effects and live (AUX's, the mic's, USB's) and the
    // gains: the hand's and SysEx's, not MIDI's CCs
    bool saved_dry_[kNumSources] = {false, false, false};
    bool saved_live_[kNumSources] = {true, false, true};
    float saved_gain_[kNumSources] = {.75f, .75f, .75f};
    bool usb_audio_ = true;
    uint8_t held_ = 0;          // SHIFT, VOLUME's press and LOOP down on the page, and the
                                // play page's SHIFT, by Held's bits
    uint32_t held_since_ = 0;   // since when both are
    bool reported_ = false;     // this hold has asked for its bug report
    uint32_t card_since_ = 0;   // since when SHIFT and LOOP are
    bool card_asked_ = false;   // this hold has asked for the card
    int unsaved_key_ = kNoKey;  // the key that changed a setting last, not yet on the card
    LedSignal refused_;         // its write failed: 3 red blinks on it
    LedSignal dark_fx_;         // A# pressed while dark: 3 red blinks on it
    LedSignal no_usb_switch_;   // SHIFT + F# with the card lent: 3 red blinks on F#
};

constexpr const float* SettingsPage::kFactorColors[];
constexpr const float* SettingsPage::kSourceColors[];
constexpr const float* SettingsPage::kInSourceColors[];
constexpr uint8_t SettingsPage::kKnobLeds[];
constexpr const float* SettingsPage::kBatteryColors[];

} // namespace chompi

#pragma GCC pop_options
