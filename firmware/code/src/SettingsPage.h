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
 *    F# (upper octave)   mono input, on / off                white, lit when on
 *    G# (upper octave)   the LEDs' brightness, each press the next: 100, 75, 50 %
 *                                                            purple, dimmed as all are
 *    A# (upper octave)   the input, each press the next: AUX through the effects, AUX dry
 *                        (past them and the compressor), the built-in mic (#152)
 *                                                            green, white, magenta
 *
 *  AUX's jack picks AUX or the mic at power-on and whenever a cable goes in or out
 *  (chompi_main.cpp), AUX in the variant chosen last, which is saved; the key picks the next
 *  until then. The source itself isn't saved. Each source has its own input gain, saved too:
 *  VOLUME's page 2, here and on the play page, sets the one heard.
 *
 *    VOLUME              turned: page 1 the output, page 2 the input gain, as on the play
 *                        page (NormalPage.h, which handles it); a press, page 1 or 2; no
 *                        page 3, and SHIFT + turn does nothing here
 *    SHIFT + VOLUME press, both held 2 s     a bug report (EventLog.h), once per hold;
 *                                            the transport LEDs blink as on the play page
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
#include "MasterSettings.h"
#include "MidiClock.h"
#include "MidiControl.h"
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
        quarters_ = master.led_brightness / 25;
        SetLedQuarters(quarters_);
        engine_->SetAuxDry(master.aux_dry);
        SetGain(false, master.gain_aux / 100.f);
        SetGain(true, master.gain_mic / 100.f);
    }

    /** A key going down on the page. True if it changed a setting, which then goes to the card */
    bool Key(int key)
    {
        if (key == static_cast<int>(kInputKey))
        {
            // AUX through the effects, AUX dry, the mic, ...: AUX's variant goes to the card
            using Input = PassthroughEngine::Input;
            const Input in = engine_->GetInput();
            if (!SetInput(in == Input::AUX_FX ? Input::AUX_DRY : in == Input::AUX_DRY ? Input::MIC : Input::AUX_FX))
                return false;
        }
        else if (!Apply(key))
            return false;
        unsaved_key_ = key;
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
        if (key == static_cast<int>(kMonoKey))
            return SetMono(!mono_);
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
        if (key == static_cast<int>(kBrightnessKey))
        {
            // 100, 75, 50 %, 100 ...
            quarters_ = quarters_ > 2 ? quarters_ - 1 : 4;
            SetLedQuarters(quarters_);
            return true;
        }
        return false;
    }

public:
    /** A key going down or up on the page: SHIFT (CHOMPI) and VOLUME's press are watched
     *  for the bug report's hold */
    void Held(int key, bool down, uint32_t now)
    {
        const uint8_t bit = key == static_cast<int>(Hardware::SwId::KEY_26)     ? 1
                            : key == static_cast<int>(Hardware::SwId::ENC_6_SW) ? 2
                                                                                : 0;
        if (!bit)
            return;
        const uint8_t was = held_;
        held_ = down ? held_ | bit : held_ & ~bit;
        if (held_ == kBothHeld && was != kBothHeld)
            held_since_ = now;
        else if (held_ != kBothHeld)
            reported_ = false;
    }

    /** True once per hold, when SHIFT and VOLUME's press have both been held kBugReportHoldMs */
    bool BugReportHeld(uint32_t now)
    {
        if (held_ != kBothHeld || reported_ || now - held_since_ < kBugReportHoldMs)
            return false;
        reported_ = true;
        return true;
    }

    /** The input (CC 84 too, which the card doesn't get): true if AUX's variant changed */
    bool SetInput(PassthroughEngine::Input in)
    {
        using Input = PassthroughEngine::Input;
        engine_->SetMicInput(in == Input::MIC);
        if (in == Input::MIC || engine_->AuxDry() == (in == Input::AUX_DRY))
            return false;
        engine_->SetAuxDry(in == Input::AUX_DRY);
        return true;
    }

    /** The input gain of the source heard now, 0..1, and setting it: VOLUME's page 2 */
    inline float InGain() const { return gain_[engine_->MicInput()]; }
    inline void SetInGain(float gain) { SetGain(engine_->MicInput(), gain); }

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
        MasterSettings master;
        Store(master);
        *Field(master, kLines[line]) = static_cast<uint8_t>(kLines[line].kind == Line::SWITCH ? v != 0 : v);
        // a setter leaves a setting that stays as it is (the clock keeps its state)
        Init(engine_, midi_, hw_, master);
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
        master.led_brightness = quarters_ * 25;
        master.aux_dry = engine_->AuxDry();
        master.gain_aux = static_cast<uint8_t>(gain_[0] * 100.f + .5f);
        master.gain_mic = static_cast<uint8_t>(gain_[1] * 100.f + .5f);
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
        KeyLed(kMonoKey, white, mono_);
        // the factor's colour, on the beats of the tempo the effects follow (TempoClock.h)
        const bool beat = engine_->FxClockPosition() % kPulsesPerBeat < kPulsesPerBeat / 2;
        KeyLed(kFactorKey, kFactorColors[static_cast<uint8_t>(factor_)], beat);
        KeyLed(kSourceKey, kSourceColors[static_cast<uint8_t>(midi_->GetClockSource())], true);
        const MidiOutPorts out = midi_->GetMidiOut();
        KeyLed(kMidiOutKey, out == MidiOutPorts::TRS ? orange : white, out != MidiOutPorts::OFF);
        KeyLed(kBlockTrsKey, med_blue, midi_->BlockTrs());
        KeyLed(kFeedbackKey, med_blue, midi_->Feedback());
        KeyLed(kBrightnessKey, purple, true); // dimmed with every LED: it shows itself
        KeyLed(kInputKey, kInputColors[static_cast<uint8_t>(engine_->GetInput())], true);

        const unsigned level = hw_->GetBatteryLevel();
        const float* battery = level < 4 ? kBatteryColors[level] : green;
        PthLed(kVolumeLed, battery, 1.f);
        PthLed(kTransportLedRev, purple, 1.f);
        PthLed(kTransportLedFwd, purple, 1.f);
        PthLed(kChompiKeyLed, white, shift || (held_ & kShiftHeld) ? 1.f : 0.f);
        if (refused_.Active(now))
            SmtLed(KeyLedOf(static_cast<Hardware::SwId>(unsaved_key_)), red,
                   refused_.BlinkLit(now) ? 1.f : 0.f);
    }

private:
    void SetGain(bool mic, float gain)
    {
        gain_[mic] = gain < 0.f ? 0.f : (gain > 1.f ? 1.f : gain);
        engine_->SetInputGain(mic, gain_[mic]);
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

    /** A key's SMT LED: the white keys right to left from 24, the dark keys left to right
     *  from 0 (FxSlots.h, NormalPage.h's scene keys) */
    static uint8_t KeyLedOf(Hardware::SwId key)
    {
        const int k = static_cast<int>(key);
        for (uint8_t w = 0; w < 15; w++)
            if (k == static_cast<int>(kWhiteKeys[w]))
                return 24 - w;
        for (uint8_t d = 0; d < 10; d++)
            if (k == static_cast<int>(kDarkKeys[d]))
                return d;
        return 0;
    }

    static const uint8_t kNumChannelKeys = 14; // white keys 1-14; the 15th is 15 and 16
    static const Hardware::SwId kChannel15Key = Hardware::SwId::KEY_15;
    static const uint8_t kShiftHeld = 1, kBothHeld = 3; // Held's bits: SHIFT 1, VOLUME 2
    static const int kNoKey = -1;
    static const uint32_t kBugReportHoldMs = 2000;

    // the dark keys by topic (#97): MIDI in on the lower octave, from all channels (KEY_16)
    // on; the rest on the upper
    static const Hardware::SwId kBlockTrsKey = Hardware::SwId::KEY_17;
    static const Hardware::SwId kTransportKey = Hardware::SwId::KEY_18;
    static const Hardware::SwId kFactorKey = Hardware::SwId::KEY_19;
    static const Hardware::SwId kSourceKey = Hardware::SwId::KEY_20;
    static const Hardware::SwId kMidiOutKey = Hardware::SwId::KEY_21;
    static const Hardware::SwId kFeedbackKey = Hardware::SwId::KEY_22;
    static const Hardware::SwId kMonoKey = Hardware::SwId::KEY_23;
    static const Hardware::SwId kBrightnessKey = Hardware::SwId::KEY_24;
    static const Hardware::SwId kInputKey = Hardware::SwId::KEY_25;
    // the factor's hue, by ClockFactor: x1/2 light blue, x1 yellow, x2 red
    static constexpr const float* kFactorColors[3] = {med_blue, yellow, red};
    // the source's, by ClockSource: Auto white, TRS orange, USB blue, internal pink
    static constexpr const float* kSourceColors[kNumClockSources] = {white, orange, blue, pink};
    // the input's, by PassthroughEngine::Input: AUX through the effects green, AUX dry white,
    // the mic magenta
    static constexpr const float* kInputColors[3] = {green, white, magenta};
    static constexpr Hardware::SwId kWhiteKeys[15] = {
        Hardware::SwId::KEY_1,  Hardware::SwId::KEY_2,  Hardware::SwId::KEY_3,
        Hardware::SwId::KEY_4,  Hardware::SwId::KEY_5,  Hardware::SwId::KEY_6,
        Hardware::SwId::KEY_7,  Hardware::SwId::KEY_8,  Hardware::SwId::KEY_9,
        Hardware::SwId::KEY_10, Hardware::SwId::KEY_11, Hardware::SwId::KEY_12,
        Hardware::SwId::KEY_13, Hardware::SwId::KEY_14, Hardware::SwId::KEY_15};
    static constexpr Hardware::SwId kDarkKeys[10] = {
        Hardware::SwId::KEY_16, Hardware::SwId::KEY_17, Hardware::SwId::KEY_18,
        Hardware::SwId::KEY_19, Hardware::SwId::KEY_20, Hardware::SwId::KEY_21,
        Hardware::SwId::KEY_22, Hardware::SwId::KEY_23, Hardware::SwId::KEY_24,
        Hardware::SwId::KEY_25};
    // the battery's level, by Hardware::BatteryLevel
    static constexpr const float* kBatteryColors[4] = {white, green, yellow, red};

    PassthroughEngine* engine_ = nullptr;
    MidiControl* midi_ = nullptr;
    Hardware* hw_ = nullptr;
    bool mono_ = false;
    ClockFactor factor_ = ClockFactor::ONE;
    uint8_t quarters_ = 4;
    float gain_[2] = {.75f, .75f}; // the input gain, AUX's and the mic's
    uint8_t held_ = 0;          // SHIFT and VOLUME's press down on the page, by Held's bits
    uint32_t held_since_ = 0;   // since when both are
    bool reported_ = false;     // this hold has asked for its bug report
    int unsaved_key_ = kNoKey;  // the key that changed a setting last, not yet on the card
    LedSignal refused_;         // its write failed: 3 red blinks on it
};

constexpr const float* SettingsPage::kFactorColors[];
constexpr const float* SettingsPage::kSourceColors[];
constexpr const float* SettingsPage::kInputColors[];
constexpr Hardware::SwId SettingsPage::kWhiteKeys[];
constexpr Hardware::SwId SettingsPage::kDarkKeys[];
constexpr const float* SettingsPage::kBatteryColors[];

} // namespace chompi

#pragma GCC pop_options
