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
 *    C# (lower octave)   all channels                        light blue
 *    D#                  MIDI transport following, on / off  green, lit when on
 *    F#                  mono input, on / off                white, lit when on
 *    G#                  the clock's tempo factor, each press the next: x1, x2, x1/2
 *                                                            yellow, red, light blue,
 *                        lit for the first half of every beat the effects follow, else dim
 *    A#                  the LEDs' brightness, each press the next: 100, 75, 50 %
 *                                                            purple, dimmed as all are
 *    D# (upper octave)   MIDI out, each press the next: off, the jack, the jack and USB
 *                                                            dim, orange, white
 *    G# (upper octave)   the clock source, each press the next: Auto, TRS, USB, internal
 *                                                            white, orange, blue, pink
 *
 *  The upper octave's other dark keys are free.
 *
 *    SHIFT + VOLUME press, both held 2 s     a bug report (EventLog.h), once per hold;
 *                                            the transport LEDs blink as on the play page
 *
 *  Each key acts on its press. A key that went down on the play page stays the play page's
 *  until it's let go, and the other way round (ui.h), so flipping the switch with a key held
 *  doesn't let go of it. A scene mode (SAVE, COPY, DELETE) is left as the switch goes up.
 *  VOLUME's LED shows the battery's level throughout (Hardware's
 *  BatteryLevel: white while the charging cable is in, green above 3.3 V, yellow below, red
 *  below 3 V); the transport LEDs are purple, so the page is never taken for the play page.
 *
 *  Each setting goes to the card as the master settings do (MasterSettings.h).
 */
#pragma once
#include "hardware.h"
#include "LedColors.h"
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
        quarters_ = master.led_brightness / 25;
        SetLedQuarters(quarters_);
    }

    /** A key going down on the page. True if it changed a setting, which then goes to the card */
    bool Key(int key)
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
        if (key == static_cast<int>(S::KEY_17))
        {
            midi_->SetTransport(!midi_->Transport());
            return true;
        }
        if (key == static_cast<int>(S::KEY_18))
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

    /** The mono input, from its key or MIDI (CC 60). True if it changed */
    bool SetMono(bool mono)
    {
        if (mono == mono_)
            return false;
        mono_ = mono;
        engine_->SetMonoInput(mono_);
        return true;
    }
    inline bool Mono() const { return mono_; }

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
        master.led_brightness = quarters_ * 25;
    }

    /** Every LED; the panel's were cleared */
    void Draw()
    {
        SmtLedsOff();

        const uint8_t channel = midi_->Channel();
        for (uint8_t ch = 0; ch < kNumChannelKeys; ch++)
            KeyLed(kWhiteKeys[ch], med_blue, channel == ch + 1);
        KeyLed(kChannel15Key, channel == 16 ? white : med_blue, channel >= 15);
        KeyLed(Hardware::SwId::KEY_16, med_blue, channel == 0);
        KeyLed(Hardware::SwId::KEY_17, green, midi_->Transport());
        KeyLed(Hardware::SwId::KEY_18, white, mono_);
        // the factor's colour, on the beats of the tempo the effects follow (TempoClock.h)
        const bool beat = engine_->FxClockPosition() % kPulsesPerBeat < kPulsesPerBeat / 2;
        KeyLed(kFactorKey, kFactorColors[static_cast<uint8_t>(factor_)], beat);
        KeyLed(kSourceKey, kSourceColors[static_cast<uint8_t>(midi_->GetClockSource())], true);
        const MidiOutPorts out = midi_->GetMidiOut();
        KeyLed(kMidiOutKey, out == MidiOutPorts::TRS ? orange : white, out != MidiOutPorts::OFF);
        KeyLed(kBrightnessKey, purple, true); // dimmed with every LED: it shows itself

        const unsigned level = hw_->GetBatteryLevel();
        const float* battery = level < 4 ? kBatteryColors[level] : green;
        PthLed(kVolumeLed, battery, 1.f);
        PthLed(kTransportLedRev, purple, 1.f);
        PthLed(kTransportLedFwd, purple, 1.f);
    }

private:
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
    static const uint8_t kBothHeld = 3;          // Held's bits: SHIFT 1, VOLUME 2
    static const uint32_t kBugReportHoldMs = 2000;

    // G# and A# of the lower octave
    static const Hardware::SwId kFactorKey = Hardware::SwId::KEY_19;
    static const Hardware::SwId kBrightnessKey = Hardware::SwId::KEY_20;
    // G# of the upper octave, where the factor's is in the lower
    static const Hardware::SwId kSourceKey = Hardware::SwId::KEY_24;
    // D# of the upper octave, where transport following's is in the lower
    static const Hardware::SwId kMidiOutKey = Hardware::SwId::KEY_22;
    // the factor's hue, by ClockFactor: x1/2 light blue, x1 yellow, x2 red
    static constexpr const float* kFactorColors[3] = {med_blue, yellow, red};
    // the source's, by ClockSource: Auto white, TRS orange, USB blue, internal pink
    static constexpr const float* kSourceColors[kNumClockSources] = {white, orange, blue, pink};
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
    uint8_t held_ = 0;          // SHIFT and VOLUME's press down on the page, by Held's bits
    uint32_t held_since_ = 0;   // since when both are
    bool reported_ = false;     // this hold has asked for its bug report
};

constexpr const float* SettingsPage::kFactorColors[];
constexpr const float* SettingsPage::kSourceColors[];
constexpr Hardware::SwId SettingsPage::kWhiteKeys[];
constexpr Hardware::SwId SettingsPage::kDarkKeys[];
constexpr const float* SettingsPage::kBatteryColors[];

} // namespace chompi

#pragma GCC pop_options
