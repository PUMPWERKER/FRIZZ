/** @file NormalPage.h
 *  @brief The main play-mode UiPage (see ui.h): VOLUME, the looper's keys and transport, the
 *  punch-in FX keys and knobs (FxControls.h, FxSlots.h) and the FX scenes (SceneControls.h,
 *  SceneStore.h), with their LEDs. The logic is in FxControls.h and SceneControls.h; this
 *  routes the hardware to it and draws. MANUAL.md describes every control; what's here is what the manual doesn't say.
 *
 *  SHIFT is the CHOMPI key held. VOLUME's page 3 sets the headphone feed, from the master out
 *  to the dry input (passthroughEngine.h). Its page 2 sets the input gain of the source heard
 *  (SettingsPage.h keeps one per source, on the card); its LED there is the input's meter, red
 *  where the loop's soft limit takes it (Looper.h's kLoopCeiling), the gain's colour for
 *  kGainShownMs after a turn. The mode switch up shows the settings page instead
 *  (SettingsPage.h, ShowSettings): the hand's keys go there, while MIDI's still come here,
 *  and its settings go to the master file as the compressor's knobs do. VOLUME works there
 *  too, handled here: its turns and presses, pages 1 and 2 only.
 *
 *  The CHOMPI, PLAY and LOOP keys' rules are in PlayKeys.h: SHIFT, the confirm tap in a scene
 *  mode, the looper's combos, tap tempo (TapTempo.h). This page is its Host. Every other key
 *  or knob used tells it (Used, FxKey), so SHIFT + it is a combo rather than a confirm.
 *
 *  FX keys: an FX key held, then SHIFT, toggles the latch, decided on the key's release;
 *  SHIFT, then an FX key, only selects it for the knobs, silently (FxControls.h), and the key
 *  flashes white. Knobs 1-4 edit the FX pressed or selected last. Which came first is judged
 *  with kShiftGraceMs of grace (#115), by when the audio callback saw the keys (KeyEdge), not
 *  when MainLoop gets to their events: SHIFT going down that soon after an FX key's or the
 *  compressor's press, or let go that soon before it, is SHIFT first, a select
 *  (FxControls::LateSelect); the latch gesture leaves far more time between them. An FX key
 *  back down that soon after its release is a bounce, and takes the release back
 *  (FxControls::Rebound).
 *
 *  The master compressor's key (kCompKey) selects it for the knobs, and flashes white; it's
 *  always on, so the key does nothing else. Its knobs go to the card (SceneStore's master
 *  file) kMasterSaveDelayMs after the last turn, so a sweep is one write.
 *
 *
 *  Scenes: a recall sends only the parameters that change, within one audio block and at the
 *  fast slew (FxChain::FastSlew), so an FX the two scenes share runs on untouched. SHIFT +
 *  a scene key morphs to it instead (FxMorph.h), landing on a bar line; the engine times it.
 *  SHIFT + a scene key, then PLAY drops to it hard on the bar line (#153, SceneControls::Drop),
 *  its key blinking red on the beat until then; SHIFT + PLAY without a scene key tapped stops a
 *  morph where it is; without one, PLAY works as ever. The transport knob
 *  turned while SHIFT still holds a morph is its crossfader (#66, SceneControls::FaderTurned):
 *  the glide in the hand, until SHIFT is let go (ReleaseMorph); the transport LEDs show it.
 *  A morph on MIDI's crossfader (CC 80) stays MIDI's: letting go of SHIFT doesn't end it.
 *  The card is written from MainLoop (SceneStore::Process), never here.
 *
 *  MIDI (MidiControl.h): notes and FRIZZ's SysEx keys and detents come as the hand's do (ui.h);
 *  the rest, Remote() takes from MainLoop: a controller sets a parameter, a latch, a gain, the
 *  mix or the loop's speed outright, as its knob or key would, a program change is a scene key,
 *  a morph over MIDI glides at once over the bars set; the compressor's CCs aren't kept on the
 *  card. The loop follows MIDI's transport in the audio callback (Looper::Follow). It answers
 *  SysEx queries over USB.
 */
#pragma once

#include "FxControls.h"
#include "FxSlots.h"
#include "hardware.h"
#include "LedSignal.h"
#include "PanelLeds.h"
#include "PlayKeys.h"
#include "SceneControls.h"
#include "SettingsPage.h"
#include "TapTempo.h"
#include "SceneStore.h"
#include "EventLog.h"
#include "MidiControl.h"
#include "LedColors.h"
#include "passthroughEngine.h"
#include "temp_led_stuff.h"

// the play page's logic, a key or a frame now and then and never per sample: built for size,
// as FRIZZ's code space is tight (SRAM_EXEC, docs/CAPACITY.md)
#pragma GCC push_options
#pragma GCC optimize("Os")

namespace chompi
{
    // per detent: the gains 2%, the mix (SHIFT) 4%, the headphone feed 1% like the FX knobs
    static const float kVolumeStep = .02f;
    static const float kMixStep = .04f;
    static const float kHpCueStep = .01f;
    // the hand's keys while the mode switch is up come as these IDs plus their SwId (ui.h)
    static const uint16_t kSettingsKeyBase = 64;

    static const float kDefaultOutGain = .75f;
    static const float kDefaultMix = 0.f; // fully dry at power-on, nothing recorded yet

    // VOLUME's pages, in the order a press steps through them
    enum VolumePage : uint8_t
    {
        kOutGainPage,
        kInGainPage,
        kHpCuePage,
        kNumPages,
    };

    // encoder IDs, by ui.h's encoder_map: 0-3 are knobs 1-4
    static const uint16_t kTransportEncoder = 4;
    static const uint16_t kVolumeEncoder = 5;

    static const float kPausedDim = .3f;
    // the beat on the PLAY LED (#110): the first half of each beat this white with no loop
    // playing; while one plays, a flash this much whiter (or darker) on each beat, more on a
    // bar's 1
    static const float kBeatPulse = .25f;
    static const float kBeatFlash = .35f, kDownbeatFlash = .7f;
    static const uint32_t kBeatFlashMs = 50;
    // waiting for something: a quantized record closing, an erase at the loop's end, a picked
    // scene slot, the CHOMPI key armed to confirm. Slower than a refusal's 3 blinks (LedSignal)
    static const uint32_t kPendingBlinkMs = 250;
    static const uint32_t kTapFlashMs = 80;      // LOOP flashes on a tempo tap
    static const uint32_t kSelectFlashMs = 80;   // an FX key flashes on a select
    static const uint32_t kShiftGraceMs = 15;    // SHIFT this near an FX key's press: first (#115)
    static const float kFlashDarkAbove = .5f;    // a flash goes dark on an LED this white (Flash)
    static const float kSemiStepPerTurn = .5f;   // 2 transport detents per semitone
    static const float kSpeedStepPerTurn = .25f; // SHIFT: 4 detents per step of the ladder

    static const uint8_t kFxKnobLeds[kNumFxKnobs] = {1, 2, 3, 4}; // PTH LEDs of knobs 1-4
    // page 2's knob 4, Level: its LED a VU meter of the effect's output (FxVu)
    static const size_t kVuKnob = kNumFxKnobs - 1;
    // Knob 1-4 press switches, by ui.h's encoder_map
    static const Hardware::SwId kFxKnobSwitches[kNumFxKnobs] = {
        Hardware::SwId::ENC_4_SW,
        Hardware::SwId::ENC_1_SW,
        Hardware::SwId::ENC_2_SW,
        Hardware::SwId::ENC_3_SW,
    };
    // FX key LEDs: off, every FX key dimly in its colour (kFxOffLevel, PanelLeds.h)
    static const float kFxMeterFloorDb = -30.f; // the meters' range, up to 0 dBFS
    static const float kFxWhiteMax = .8f;     // on: how far the loudest audio pushes to white
    static const float kCompMeterDb = 12.f;   // the compressor key's full brightness, dB reduced
    // A bipolar knob's LED (KnobColor): blue below its neutral point, white on it, orange above
    static constexpr const float* kBipolarLow = blue;
    static constexpr const float* kBipolarHigh = orange;
    // A split knob's upper, stepped half (FxParams::split, the phaser's rate synced to the
    // tempo): green on its first step to yellow on its last, the same on every effect
    static constexpr const float* kSplitLow = green;
    static constexpr const float* kSplitHigh = yellow;
    // The safety limiter on the compressor's key: red from kLimStartDb of limiting, fully red
    // at kLimFullDb, held kLimHoldMs so a short peak is seen
    static constexpr float kLimStartDb = 1.f, kLimFullDb = 3.f;
    static const uint32_t kLimHoldMs = 300;
    static const uint32_t kMasterSaveDelayMs = 2000;
    static const uint32_t kMasterSaveTries = 3; // a failed write is tried again, this often

    // FX scenes (#67): the slots on KEY_18-22, the lower F#, G#, A# and the upper C#, D#, the
    // blank one first; SAVE / COPY / DELETE on the upper F#, G#, A#, save first, delete on the
    // outside in red (TAPE's and TEMPO's preset keys go the other way). The lower C# and D# are
    // free on this page. One language for all three: the mode's colour shows what will happen
    // (the slots it can act on, the pick, the CHOMPI key that confirms), white that it's done,
    // red that it was refused or isn't on the card
    struct SceneKey
    {
        Hardware::SwId key;
        uint8_t led;
    };
    static const SceneKey kSceneKeys[kNumSlots] = {
        {Hardware::SwId::KEY_18, 2},
        {Hardware::SwId::KEY_19, 3},
        {Hardware::SwId::KEY_20, 4},
        {Hardware::SwId::KEY_21, 5},
        {Hardware::SwId::KEY_22, 6},
    };
    struct SceneModeKey
    {
        SceneMode mode;
        Hardware::SwId key;
        uint8_t led;
        const float* color;
    };
    static const SceneModeKey kSceneModeKeys[] = {
        {SceneMode::SAVE, Hardware::SwId::KEY_23, 7, green},
        {SceneMode::COPY, Hardware::SwId::KEY_24, 8, blue},
        {SceneMode::DELETE, Hardware::SwId::KEY_25, 9, red},
    };
    static const uint32_t kScenePulseMs = 1000;     // the active scene, edited

    static const float kVuDim[3] = {.1f, .1f, .1f}; // the VU meter's floor
    // the input's meter on VOLUME's page 2: dim to green to yellow from kInMeterFloorDb to the
    // loop's ceiling, red at it, held kInRedMs; the gain's colour kGainShownMs after a turn
    static const float kInMeterFloorDb = -40.f;
    static const float kInMeterFall = .9f; // a frame's, ~20 ms each
    static const uint32_t kInRedMs = 300;
    static const uint32_t kGainShownMs = 1000;
    // the settings page's VOLUME LED shows its page this long after a turn or press, then
    // the battery again
    static const uint32_t kSettingsVolumeMs = 2000;

    class NormalPage : public daisy::UiPage
    {
    public:
        void Init(PassthroughEngine *engine, Hardware *hw, SceneStore *scenes, EventLog *log,
                  MidiControl *midi)
        {
            hw_ = hw;
            log_ = log;
            midi_ = midi;
            engine_ = engine;
            scenes_ = scenes;

            page_ = kOutGainPage;

            // the engine only hears about a value when it changes, so push them all now
            SetOutGain(kDefaultOutGain);
            SetMix(kDefaultMix);
            SetHpCue(0.f); // the headphones mirror the master at power-on

            fx_.Init(engine_);
            // the compressor's knobs as they were left, from the card
            for (size_t p = 0; p < kNumFxParams; p++)
                fx_.SetComp(p, scenes_->master.comp[p]);
            fx_.TakeCompChanged();
            // and the settings page's: the mono input, the clock's factor, the LEDs, the input
            // gains and AUX's variant
            settings_.Init(engine_, midi_, hw_, scenes_->master);
            scene_ctl_.Init(scenes_->scenes, &fx_);
            keys_.Init(this);

            ResetSmtLeds();
            PthLedsOff();
        }

        /** Before a restart: master settings waiting out their kMasterSaveDelayMs go to the
         *  card at once, the first time it's asked (a change after that, automation still
         *  coming, waits as usual). True once there's nothing left to write */
        bool MasterSettled()
        {
            if (master_unsaved_ && !master_hurried_)
                master_changed_at_ = System::GetNow() - kMasterSaveDelayMs - 1;
            master_hurried_ = true;
            return !master_unsaved_;
        }

        // called from the audio callback (ui.h's GenerateEvents, and DoEvents while it boots):
        // built as the callback is, so it inlines them rather than calling -Os copies
#pragma GCC pop_options
        /** From ui.h, as the mode switch goes: up shows the settings page */
        inline void ShowSettings(bool show) { show_settings_ = show; }
        /** From ui.h, a key of this page going down or up, when the audio callback saw it */
        inline void KeyEdge(int key, bool down, uint32_t now)
        {
            if (key < kEdgeKeys)
                edge_ms_[down][key] = now;
        }

        inline void ResetSmtLeds() { SmtLedsOff(); }
#pragma GCC push_options
#pragma GCC optimize("Os")

        void Draw(const daisy::UiCanvasDescriptor &canvasDescriptor) override
        {
            const uint32_t now = System::GetNow();
            Update(now);

            // the boot / rainbow animations leave the other knob LEDs lit, and nothing
            // clears the canvas, so blank them all every frame
            PthLedsOff();

            // the keys the play page doesn't draw would keep the settings page's colours; a
            // scene mode (SAVE, COPY, DELETE) is left on the way up, so CHOMPI can't confirm
            // it later. The switch is read once: the audio callback may flip it meanwhile
            const bool settings = show_settings_;
            if (settings != drew_settings_)
            {
                drew_settings_ = settings;
                ResetSmtLeds();
                if (settings)
                    scene_ctl_.Cancel();
            }
            if (settings)
            {
                // SHIFT + VOLUME held there: a bug report, its blink over the transport LEDs
                if (settings_.BugReportHeld(now))
                    log_->RequestWrite();
                settings_.Draw(now, Shift());
                // VOLUME's page for a while after it was touched, over the battery
                const bool volume = now - vol_touched_ < kSettingsVolumeMs;
                MeterInput(now, volume && SettingsVolumePage() == kInGainPage);
                if (volume)
                    DrawVolumeLed(now, SettingsVolumePage());
                DrawLogLeds(now);
                fill_led_data();
                return;
            }

            MeterInput(now, page_ == kInGainPage);
            if (Shift())
            {
                float rgb[3];
                Xfade(green, purple, mix_, rgb);
                SetPthLedFloat(kVolumeLed, rgb[0], rgb[1], rgb[2]);
            }
            else
                DrawVolumeLed(now, page_);

            DrawLooperLeds(now);
            DrawLogLeds(now);
            DrawFxLeds(now);
            DrawSceneLeds(now);

            // CHOMPI key: blinking in the mode's colour while a tap would confirm a scene
            // action, otherwise white while it is acting as SHIFT
            if (scene_ctl_.Armed() && !keys_.ShiftCombo())
                PthLed(kChompiKeyLed, SceneModeColor(), BlinkOn(now, kPendingBlinkMs) ? 1.f : 0.f);
            else
                PthLed(kChompiKeyLed, white, Shift() ? 1.f : 0.f);

            fill_led_data();
        }

        bool OnButton(uint16_t buttonID,
                      uint8_t numberOfPresses,
                      bool isRetriggering) override
        {
            const bool rising = numberOfPresses == 1;
            if (buttonID >= kSettingsKeyBase)
            {
                const int key = buttonID - kSettingsKeyBase;
                const uint32_t now = System::GetNow();
                settings_.Held(key, rising, now);
                if (key == static_cast<int>(Hardware::SwId::ENC_6_SW))
                    SettingsVolumePressed(rising, now);
                else if (rising && settings_.Key(key))
                    MasterChanged(now);
                return true;
            }
            switch (buttonID)
            {
            case static_cast<uint16_t>(Hardware::SwId::KEY_26):
            {
                // a release whose press the boot page had lets go of nothing
                const bool held = Shift();
                if (rising)
                    scene_ctl_.ShiftDown();
                keys_.Chompi(rising);
                if (!rising && held)
                    ReleaseMorph();
                return true;
            }
            case static_cast<uint16_t>(Hardware::SwId::KEY_27):
                keys_.Play(rising);
                return true;
            case static_cast<uint16_t>(Hardware::SwId::KEY_28):
                keys_.Loop(rising);
                return true;
            default:
                break;
            }

            if (buttonID == static_cast<uint16_t>(kCompKey))
            {
                // the compressor is always on: its key only selects, and always flashes
                if (rising)
                {
                    FxKeyDown(true);
                    fx_.CompKeyPressed(ShiftAt(buttonID));
                }
                return true;
            }
            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                if (buttonID == static_cast<uint16_t>(kFxSlots[fx].key))
                {
                    if (!rising)
                        fx_.KeyPressed(fx, false, Shift());
                    else if (!(Grace(buttonID, false, buttonID) && fx_.Rebound(fx)))
                    {
                        const bool shift = ShiftAt(buttonID);
                        FxKeyDown(shift);
                        fx_.KeyPressed(fx, true, shift);
                    }
                    return true;
                }
            }

            // VOLUME: a press picks the next page, on its release. SHIFT + press resets the mix, as SHIFT + press does on knobs 1-4, to where a recording
            // or an erase leaves it: the loop only while there is one, otherwise the input
            // only. Whether it was SHIFT + press is decided on the press, so letting go of
            // CHOMPI first doesn't turn its release into a page change
            if (buttonID == static_cast<uint16_t>(Hardware::SwId::ENC_6_SW))
            {
                if (rising)
                {
                    keys_.Used();
                    vol_down_ = true;
                    vol_shift_ = Shift();
                    if (vol_shift_)
                        SetMix(LoopExists() ? 1.f : 0.f);
                }
                else
                {
                    // a release whose press the boot page had picks no page
                    if (vol_down_ && !vol_shift_)
                    {
                        page_ = (page_ + 1) % kNumPages;
                        page_flash_.Start(System::GetNow(), (page_ + 1) * 2 * kSignalBlinkMs);
                    }
                    vol_down_ = false;
                }
                return true;
            }
            if (!rising)
                return true;

            // The keys and presses below act only on the way down. Only those that do
            // something count as a SHIFT combo (keys_.Used): a key without a function doesn't
            // cancel a confirm or a latch in the making.

            // transport press: back to 1x forward; SHIFT + press does nothing (it wrote a bug
            // report until v0.11, now held on the settings page)
            if (buttonID == ENC_5_SW)
            {
                if (!Shift() && LoopExists())
                {
                    keys_.Used();
                    engine_->looper.ResetSpeed();
                    speed_chunk_ = 0.f;
                }
                return true;
            }
            for (size_t slot = 0; slot < kNumSlots; slot++)
            {
                if (buttonID == static_cast<uint16_t>(kSceneKeys[slot].key))
                {
                    keys_.Used();
                    ScenePressed(slot, Shift());
                    return true;
                }
            }
            for (const SceneModeKey& key : kSceneModeKeys)
            {
                if (buttonID == static_cast<uint16_t>(key.key))
                {
                    keys_.Used();
                    scene_ctl_.ModePressed(key.mode);
                    return true;
                }
            }
            for (size_t knob = 0; knob < kNumFxKnobs; knob++)
            {
                // a plain press turns the page; SHIFT + press resets, on a knob the page uses
                if (buttonID == static_cast<uint16_t>(kFxKnobSwitches[knob]))
                {
                    const size_t page = fx_.Page();
                    if (fx_.KnobPressed(knob, Shift()))
                        keys_.Used();
                    // the page turned: its number blinks on the knob LEDs, as VOLUME's do
                    if (fx_.Page() != page)
                    {
                        knob_flash_page_ = fx_.Page();
                        knob_page_flash_.Start(System::GetNow(),
                                               (knob_flash_page_ + 1) * 2 * kSignalBlinkMs);
                    }
                    return true;
                }
            }
            return true;
        }

        /** Like the keys, only a turn that does something is a SHIFT combo (keys_.Used): not
         *  the transport without a playing loop, nor a knob the FX doesn't use */
        bool OnEncoderTurned(uint16_t encoderID,
                             int16_t turns,
                             uint16_t stepsPerRevolution) override
        {
            if (encoderID == kTransportEncoder)
                TransportTurned(turns);
            else if (encoderID < kNumFxKnobs)
            {
                if (fx_.KnobUsed(encoderID))
                    keys_.Used();
                fx_.KnobTurned(encoderID, turns, Shift());
                knob_page_flash_.Stop(); // a turn shows what it turns at once
            }
            else if (encoderID == kVolumeEncoder)
            {
                if (show_settings_)
                {
                    SettingsVolumeTurned(turns);
                    return true;
                }
                keys_.Used();
                VolumeTurned(turns);
            }
            else
                return false;
            return true;
        }

        /** What came over MIDI for the page, from MainLoop: the focus, what acts once in the
         *  order it came, the latches, then the knobs' and gains' latest values, settings to
         *  keep and a query to answer */
        MIDI_CONTROL_ONCE void Remote()
        {
            const uint32_t now = System::GetNow();
            uint8_t key;
            if (midi_->TakeFocus(key))
            {
                const size_t fx = FxOnWhiteKey(key);
                fx_.Select(fx < kNumFx ? fx : kCompSelected);
                Heard(feedback::kFocus);
            }
            MidiControl::Event event;
            while (midi_->TakeEvent(event))
                RemoteEvent(event, now);
            bool on;
            while (midi_->TakeLatch(key, on))
                if (FxOnWhiteKey(key) < kNumFx)
                {
                    fx_.SetLatch(FxOnWhiteKey(key), on);
                    HeardKey(key, feedback::kLatchSlot);
                }
            uint8_t id, raw;
            while (midi_->TakeValue(id, raw))
                RemoteValue(id, raw);
            // the focus as the hand or MIDI left it, for the global channel's knobs
            const uint8_t focus = static_cast<uint8_t>(WhiteKeyOfFx(fx_.Selected()));
            if (focus != told_focus_)
            {
                told_focus_ = focus;
                midi_->SetFocus(focus);
            }

            if (midi_->TakeSettingsChanged())
                MasterChanged(now);

            // MIDI feedback switched on (or MIDI out): what stands now isn't sent
            const bool feedback = midi_->Feedback() && midi_->GetMidiOut() != MidiOutPorts::OFF;
            if (feedback && !feedback_on_)
                feedback_.Reset([this](uint8_t item, uint8_t& v) { return FeedbackValue(item, v); });
            feedback_on_ = feedback;

            // one answer a pass (every 2 ms), so USB has sent the last before the next goes,
            // or else MIDI feedback's
            MidiControl::Query query;
            MidiControl::DevQuery dev;
            if (midi_->TakeQuery(query))
                Answer(query);
            else if (midi_->TakeDev(dev))
                DevAnswer(dev);
            else if (feedback)
                Feedback(now);
        }

    private:
        /** What acts once: a morph or its stop, a tap, a program change (a scene key pressed) */
        MIDI_CONTROL_ONCE void RemoteEvent(const MidiControl::Event& e, uint32_t now)
        {
            using Event = MidiControl::Event;
            switch (e.kind)
            {
            case Event::MORPH:
                RemoteMorph(e.arg & 7, e.arg >> 3);
                break;
            case Event::DROP:
                RemoteDrop(e.arg & 7, e.arg >> 3);
                break;
            case Event::STOP_MORPH:
                FreezeMorph();
                break;
            case Event::TAP:
                // when it came: MainLoop may have been held up by the card meanwhile
                Tap(now - static_cast<uint16_t>(static_cast<uint16_t>(now) - e.ms));
                break;
            case Event::PROGRAM:
                ScenePressed(e.arg, false);
                break;
            }
        }

        /** A controller's value (midimap::Value): a knob, the compressor's too, which the card
         *  doesn't get (#97 D6), a gain, the loop's speed or the crossfader */
        MIDI_CONTROL_ONCE void RemoteValue(uint8_t id, uint8_t raw)
        {
            using namespace midimap;
            static_assert(kKeyParams == kNumFxParams, "a CC per parameter");
            const float value = MidiToKnob(raw, false);
            if (id < kOutGain)
            {
                const size_t fx = FxOnWhiteKey(id / kKeyParams), param = id % kKeyParams;
                if (fx < kNumFx)
                    fx_.SetParamTo(fx, param, value);
                else
                    fx_.SetComp(param, value, false);
                HeardKey(id / kKeyParams, param);
            }
            else if (id == kSource)
            {
                // CC 84: 0-42 AUX through the effects, 43-84 AUX dry, 85-127 the mic; as A#
                // on the settings page, but the card doesn't get it (#97)
                using Input = PassthroughEngine::Input;
                settings_.SetInput(raw < 43 ? Input::AUX_FX : raw < 85 ? Input::AUX_DRY : Input::MIC);
                Heard(feedback::kSource);
            }
            else if (id <= kHpCue)
            {
                if (id == kOutGain)
                    SetOutGain(value);
                else if (id == kInGain)
                    SetInGain(value, false); // not for the card (#97)
                else if (id == kMix)
                    SetMix(value);
                else
                    SetHpCue(1.f - value); // as VOLUME's knob: up towards the master
                Heard(static_cast<uint8_t>(feedback::kOutGain + id - kOutGain));
            }
            else if (id == kSpeed)
            {
                // semitones: 0 is 1/16x, 64 1x, 127 2x
                const int semis = raw <= 64 ? (raw * 48 + 32) / 64 - 48 : ((raw - 64) * 12 + 31) / 63;
                if (LoopExists())
                {
                    engine_->looper.SetSemitones(semis);
                    feedback_.Heard(feedback::kSpeed, feedback::SemisTo7(semis));
                }
            }
            else if (id == kFader)
            {
                ScopedIrqBlocker irq;
                scene_ctl_.FaderTo(value);
            }
        }

        // ---- MIDI feedback (MidiFeedback.h) ----

        /** A feedback item's value now: false for nothing to send (an effect's while a morph
         *  runs, an unused knob, a channel the global one hides) */
        bool FeedbackValue(uint8_t item, uint8_t& v)
        {
            using namespace feedback;
            const uint8_t channel = midi_->Channel();
            if (item < kGlobalLatches)
            {
                // an effect's channel: none on all channels, nor where the global one is
                const uint8_t key = item / kKeySlots;
                return channel && key + 1 != channel && KeyValue(key, item % kKeySlots, v);
            }
            if (item < kFocusItems)
                return KeyValue(item - kGlobalLatches, kLatchSlot, v);
            if (item < kCompItems)
                return KeyValue(WhiteKeyOfFx(fx_.Selected()), item - kFocusItems, v);
            if (item < kFocus)
                return KeyValue(WhiteKeyOfFx(kNumFx), item - kCompItems, v);
            switch (item)
            {
            case kFocus:
                v = static_cast<uint8_t>(WhiteKeyOfFx(fx_.Selected()) + 1);
                return true;
            case kOutGain:
                v = KnobTo7(out_gain_);
                return true;
            case kInGain:
                v = KnobTo7(settings_.InGain());
                return true;
            case kMix:
                v = KnobTo7(mix_);
                return true;
            case kHpCue:
                v = KnobTo7(1.f - hp_cue_);
                return true;
            case kSpeed:
                v = SemisTo7(engine_->looper.GetSemitones());
                return LoopExists();
            case kFader:
                v = KnobTo7(scene_ctl_.FaderPos());
                return hand_fader_;
            case kSource:
                v = kSourceTo7[static_cast<uint8_t>(engine_->GetInput())];
                return true;
            }
            return false;
        }

        /** White key key's knob 0-7 or latch (feedback::kLatchSlot) */
        bool KeyValue(size_t key, size_t slot, uint8_t& v)
        {
            const size_t fx = FxOnWhiteKey(key);
            if (fx == kNumFx)
            {
                v = feedback::KnobTo7(fx_.CompParam(slot < kNumFxParams ? slot : 0));
                return slot < kNumFxParams && ((kCompParams.knobs >> slot) & 1);
            }
            // a morph: where it lands goes out, once it has
            if (fx_.Morphing() || hand_fader_)
                return false;
            if (slot == feedback::kLatchSlot)
            {
                v = fx_.IsLatched(fx) ? 127 : 0;
                return true;
            }
            v = feedback::KnobTo7(fx_.Param(fx, slot));
            return (kFxParams[fx].knobs >> slot) & 1;
        }

        /** item as it stands came in by MIDI or SysEx: not sent back */
        void Heard(uint8_t item)
        {
            feedback_.Heard(item, [this](uint8_t i, uint8_t& v) { return FeedbackValue(i, v); });
        }
        /** White key key's knob or latch, every CC it goes out on */
        void HeardKey(size_t key, size_t slot)
        {
            using namespace feedback;
            Heard(static_cast<uint8_t>(key * kKeySlots + slot));
            if (slot == kLatchSlot && key < kKeys - 1)
                Heard(static_cast<uint8_t>(kGlobalLatches + key));
            if (key == WhiteKeyOfFx(fx_.Selected()))
                Heard(static_cast<uint8_t>(kFocusItems + slot));
            if (FxOnWhiteKey(key) == kNumFx && slot < kNumFxParams)
                Heard(static_cast<uint8_t>(kCompItems + slot));
        }

        /** A pass of MIDI feedback's sweep: over USB if MIDI out goes there, into the jack's
         *  ring, without the effect channels if they don't count on the jack */
        void Feedback(uint32_t now)
        {
            const uint8_t global = midi_->Channel() ? midi_->Channel() : 16;
            const bool usb = midi_->GetMidiOut() == MidiOutPorts::TRS_USB;
            const bool block_trs = midi_->BlockTrs();
            feedback_.Pass(now, global, [this](uint8_t item, uint8_t& v) { return FeedbackValue(item, v); },
                           [&](uint8_t* msg, size_t n) {
                               uint8_t trs[MidiFeedback::kPerPass * 3];
                               size_t m = 0;
                               for (size_t i = 0; i < 3 * n; i += 3)
                                   if (block_trs || (msg[i] & 0x0F) == global - 1)
                                   {
                                       trs[m++] = msg[i];
                                       trs[m++] = msg[i + 1];
                                       trs[m++] = msg[i + 2];
                                   }
                               if (midi_->feedback_trs.Room() < m)
                                   return false;
                               // USB busy: again at the next pass, a few times, then without
                               // it; without a host, not at all (#84)
                               if (usb && midi_->UsbHost() && !midi_->TrySendUsb(msg, 3 * n)
                                   && ++feedback_usb_tries_ < kFeedbackUsbTries)
                                   return false;
                               feedback_usb_tries_ = 0;
                               midi_->feedback_trs.Push(trs, m);
                               return true;
                           });
        }

        /** A morph over MIDI: as SHIFT + the scene key, tapped once per bar, and SHIFT let go,
         *  unless the hand holds it, whose release then lets it glide */
        void RemoteMorph(size_t slot, uint8_t bars)
        {
            static_assert(midimap::kMorphBarsMax == kMaxMorphBars, "MIDI's bars, the morph's");
            const auto result = ScenePressed(slot, true, bars);
            if (result != SceneControls<PassthroughEngine>::Slot::REFUSED && !Shift())
                ReleaseMorph();
        }

        /** A drop over MIDI (CC 83): as SHIFT + the scene key, tapped once per bar, then PLAY */
        void RemoteDrop(size_t slot, uint8_t bars)
        {
            bool dropped;
            {
                ScopedIrqBlocker irq;
                dropped = scene_ctl_.DropTo(slot, bars);
            }
            if (!dropped)
                SceneRefusedBlink(slot);
        }

        // the latches and keys on go out in 14 bits (Put14), a scene's latches too
        static_assert(kNumFx <= 14, "an effect's bit in 14");
        static_assert(kFxPerPart * kFxParts >= kNumFx, "a scene's parts hold every effect");

        /** A SysEx query, answered over USB (MidiControl.h has the commands) */
        MIDI_CONTROL_ONCE void Answer(const MidiControl::Query& q)
        {
            uint8_t d[MidiControl::kMaxReply];
            size_t n = 0;
            switch (q.cmd)
            {
            case kCmdState:
            {
                const Looper& looper = engine_->looper;
                uint16_t latched = 0, on = 0;
                for (size_t fx = 0; fx < kNumFx; fx++)
                {
                    latched |= fx_.IsLatched(fx) ? 1u << fx : 0u;
                    on |= fx_.IsOn(fx) ? 1u << fx : 0u;
                }
                const uint8_t flags = (scene_ctl_.Edited() ? 1 : 0)
                                      | (fx_.Morphing() ? 2 : 0) | (Shift() ? 4 : 0)
                                      | (looper.IsErasePending() ? 8 : 0)
                                      | (looper.IsClosing() ? 16 : 0)
                                      | (settings_.Mono() ? 32 : 0)
                                      | (show_settings_ ? 64 : 0);
                d[n++] = static_cast<uint8_t>(looper.GetState());
                n = Put14(d, n, KnobToMidi14((looper.GetSpeed() + 2.f) * .25f));
                d[n++] = static_cast<uint8_t>(looper.GetPosition() * 127.f);
                d[n++] = static_cast<uint8_t>(fx_.Selected());
                d[n++] = static_cast<uint8_t>(scene_ctl_.Active() + 1);
                d[n++] = flags;
                d[n++] = static_cast<uint8_t>(scene_ctl_.Mode());
                n = Put14(d, n, latched);
                n = Put14(d, n, on);
                d[n++] = page_;
                n = Put14(d, n, KnobToMidi14(mix_));
                n = Put14(d, n, KnobToMidi14(out_gain_));
                n = Put14(d, n, KnobToMidi14(settings_.InGain()));
                n = Put14(d, n, KnobToMidi14(hp_cue_));
                n = Put14(d, n, static_cast<uint16_t>(engine_->FxBpm() * 10.f + .5f));
                // the mode switch: 1 if it stands up, plus SysEx's setting (kCmdSwitch) x2
                d[n++] = static_cast<uint8_t>((hw_->GetToggleState() ? 0 : 1) | midi_->Switch() << 1);
                d[n++] = static_cast<uint8_t>(fx_.Page()); // the FX knobs' page, 0 or 1
                d[n++] = static_cast<uint8_t>(engine_->GetInput()); // AUX, AUX dry, the mic
                break;
            }
            case kCmdParams:
                // both pages of an effect's or the compressor's
                if (q.a > kNumFx)
                    return;
                d[n++] = q.a;
                for (size_t p = 0; p < kNumFxParams; p++)
                    n = Put14(d, n, KnobToMidi14(q.a == kNumFx ? fx_.CompParam(p)
                                                               : fx_.Param(q.a, p)));
                break;
            case kCmdLeds:
            {
                // part 0: the panel's 10, then the keys' 25 in three parts
                static const uint8_t kFirst[] = {0, 0, 9, 17}, kCount[] = {10, 9, 8, 8};
                if (q.a > 3)
                    return;
                d[n++] = q.a;
                for (uint8_t i = kFirst[q.a]; i < kFirst[q.a] + kCount[q.a]; i++)
                    for (int c = 0; c < 3; c++)
                        d[n++] = q.a == 0 ? led_pth_data[i][c] : led_smt_data[i][c];
                break;
            }
            case kCmdLoad:
            {
                uint16_t max, mean;
                uint32_t over, blocks;
                midi_->TakeLoad(max, mean, over, blocks);
                n = Put14(d, n, max);
                n = Put14(d, n, mean);
                n = Put7s(d, n, over, 3);
                n = Put7s(d, n, blocks, 3);
                break;
            }
            case kCmdSettings:
                d[n++] = midi_->Channel();
                d[n++] = midi_->Transport() ? 1 : 0;
                d[n++] = static_cast<uint8_t>(midi_->GetClockSource());
                d[n++] = static_cast<uint8_t>(midi_->GetMidiOut());
                break;
            case kCmdSceneGet:
                if (q.a >= kNumSlots || q.b >= kSceneParts)
                    return;
                d[n++] = q.a;
                d[n++] = q.b;
                n = ScenePart(scenes_->scenes[q.a], q.b, d, n);
                break;
            case kCmdScenePut:
                if (q.a >= kNumSlots || q.b >= kSceneParts)
                    return;
                d[n++] = q.a;
                d[n++] = q.b;
                d[n++] = TakeScenePart(q) ? 0 : 1;
                break;
            default:
                return;
            }
            midi_->Reply(q.cmd, d, n);
        }

        static size_t Put14(uint8_t* d, size_t n, uint16_t v)
        {
            d[n++] = (v >> 7) & 0x7F;
            d[n++] = v & 0x7F;
            return n;
        }

        /** v in `groups` 7-bit groups, the highest first; saturated at the most they hold */
        MIDI_CONTROL_ONCE static size_t Put7s(uint8_t* d, size_t n, uint32_t v, size_t groups)
        {
            const uint32_t top = groups >= 5 ? 0xFFFFFFFFu : (1u << (7 * groups)) - 1;
            v = v > top ? top : v;
            for (size_t g = groups; g-- > 0;)
                d[n++] = static_cast<uint8_t>(g * 7 < 32 ? (v >> (g * 7)) & 0x7F : 0);
            return n;
        }

        /** A kCmdDev request (MidiControl.h's namespace dev), answered over USB: SUB SEQ
         *  STATUS, then what it asked for. Every one is answered */
        MIDI_CONTROL_ONCE void DevAnswer(const MidiControl::DevQuery& q)
        {
            using namespace dev;
            uint8_t d[MidiControl::kMaxReply];
            size_t n = 3;
            uint8_t status = OK;
            const uint8_t group = q.len > 0 ? q.arg[0] : 0xFF;
            Info& info = midi_->dev;
            switch (q.sub)
            {
            case IDENT:
                if (!info.crc_done)
                {
                    info.crc = Crc32(info.image, info.image_len);
                    info.crc_done = true;
                }
                d[n++] = kVersion;
                d[n++] = info.kind;
                n = Put7s(d, n, info.crc, 5);
                n = Put7s(d, n, info.image_len, 3);
                n = Put7s(d, n, System::GetNow(), 5);
                n = Put7s(d, n, info.passes, 5);
                n = Put7s(d, n, info.reset_cause, 5);
                d[n++] = scenes_->InRam() ? kRamOnly : 0;
                break;
            case PERSIST:
                if (q.len < 1 || q.arg[0] != 0)
                    status = BAD;
                else
                    scenes_->KeepInRam();
                d[n++] = scenes_->InRam() ? kRamOnly : 0;
                break;
            case GET:
            {
                d[n++] = group;
                const size_t count_at = n++;
                uint16_t values[16];
                const size_t count = DevValues(group, values);
                status = count ? OK : BAD;
                for (size_t i = 0; i < count; i++)
                    n = Put14(d, n, values[i]);
                d[count_at] = static_cast<uint8_t>(count);
                break;
            }
            case SET:
                if (q.len < 4 || !DevSet(group, q.arg[1], static_cast<uint16_t>((q.arg[2] << 7) | q.arg[3])))
                    status = BAD;
                d[n++] = group;
                d[n++] = q.len > 1 ? q.arg[1] : 0;
                break;
            case BENCH:
            {
                if (!info.bench)
                {
                    status = NOT_HERE;
                    break;
                }
                const uint32_t offset = q.len >= 4 ? static_cast<uint32_t>(q.arg[1] << 14 | q.arg[2] << 7 | q.arg[3]) : 0;
                const int got = q.len >= 1 && q.arg[0] <= 1 ? info.bench(q.arg[0], offset, d + n) : 0;
                if (q.len < 1 || q.arg[0] > 1)
                    status = BAD;
                else if (got < 0)
                    status = NOT_YET;
                else
                    n += static_cast<size_t>(got);
                break;
            }
            case PROBE:
                if (group == 0)
                    n = DevClock(d, n);
                else if (group == 1)
                    n = DevLooper(d, n);
                else
                    status = BAD;
                break;
            case LEVELS:
                if (group <= 1)
                    n = DevLevels(group, d, n);
                else
                    status = BAD;
                break;
            case SIGNAL:
                if (!DevSignal(q))
                    status = BAD;
                d[n++] = midi_->audio.wave;
                break;
            case RESEED:
                if (q.len >= 5)
                {
                    info.seed = static_cast<uint32_t>(q.arg[0]) << 28 | q.arg[1] << 21 | q.arg[2] << 14
                                | q.arg[3] << 7 | q.arg[4];
                    ScopedIrqBlocker irq;
                    engine_->Reseed(info.seed);
                }
                else if (q.len)
                    status = BAD;
                n = Put7s(d, n, info.seed, 5);
                for (size_t i = 0; i < FxChain::kRandoms; i++)
                    n = Put7s(d, n, engine_->RandomState(i), 5);
                break;
            case BATTERY:
            {
                // as the last reads left it: asking reads nothing from the charger
                const uint8_t bounces[] = {hw_->vin_gd_bounce, hw_->batt_low_bounce,
                                           hw_->legacy_cable_bounce, hw_->iindpm_stat_bounce};
                d[n++] = static_cast<uint8_t>(hw_->GetBatteryLevel());
                d[n++] = static_cast<uint8_t>(hw_->batt_level);
                d[n++] = hw_->batt_check_state;
                d[n++] = hw_->read_error ? 1 : 0;
                for (uint8_t b : bounces)
                    n = Put7s(d, n, b, 2);
                for (size_t i = 0; i < 6; i++)
                    n = Put7s(d, n, hw_->mp_buff_[i], 2);
                break;
            }
            default:
                status = BAD;
            }
            d[0] = q.sub;
            d[1] = q.seq;
            d[2] = status;
            midi_->Reply(kCmdDev, d, n);
        }

        /** v * by in 7-bit groups, 0 below 0 */
        static size_t PutScaled(uint8_t* d, size_t n, float v, float by, size_t groups)
        {
            return Put7s(d, n, v > 0.f ? static_cast<uint32_t>(v * by + .5f) : 0u, groups);
        }

        /** PROBE 0: the clock, as MidiClock counts it, and the tempo the effects follow */
        MIDI_CONTROL_ONCE size_t DevClock(uint8_t* d, size_t n)
        {
            const MidiClock& clock = midi_->Clock();
            const TempoClock& tempo = engine_->Tempo();
            d[n++] = static_cast<uint8_t>(clock.GetSource());
            d[n++] = static_cast<uint8_t>(clock.Locked());
            d[n++] = static_cast<uint8_t>(clock.Factor());
            d[n++] = static_cast<uint8_t>((clock.HasClock() ? 1 : 0) | (tempo.HasLoop() ? 2 : 0)
                                          | (tempo.TempoSet() ? 4 : 0) | (tempo.Reverse() ? 8 : 0)
                                          | (midi_->Transport() ? 16 : 0));
            n = PutScaled(d, n, clock.GetBpm(), 100.f, 3);
            n = PutScaled(d, n, clock.SenderBpm(), 100.f, 3);
            n = Put7s(d, n, clock.GetTicks(), 5);
            n = Put7s(d, n, clock.GetLocks(), 3);
            n = PutScaled(d, n, tempo.GetFxBpm(), 100.f, 3);
            n = Put7s(d, n, static_cast<uint32_t>(tempo.GetTempo()), 2);
            n = Put7s(d, n, tempo.Position(), 3);
            return Put7s(d, n, tempo.LoopPulses(), 3);
        }

        /** PROBE 1: the looper */
        MIDI_CONTROL_ONCE size_t DevLooper(uint8_t* d, size_t n)
        {
            const Looper& looper = engine_->looper;
            const size_t length = looper.GetLength();
            d[n++] = static_cast<uint8_t>(looper.GetState());
            d[n++] = static_cast<uint8_t>((looper.IsClosing() ? 1 : 0) | (looper.IsErasePending() ? 2 : 0)
                                          | (looper.IsErasing() ? 4 : 0)
                                          | (looper.CanRecordQuantized() ? 8 : 0));
            n = Put7s(d, n, length, 4);
            n = PutScaled(d, n, looper.GetPosition(), static_cast<float>(length), 4);
            n = Put7s(d, n, looper.GetBeats(), 3);
            d[n++] = static_cast<uint8_t>(looper.GetSemitones() + 64);
            n = PutScaled(d, n, looper.GetSpeed() + 2.f, 1000.f, 2);
            n = PutScaled(d, n, looper.GetActualSpeed() + 2.f, 1000.f, 2);
            return Put7s(d, n, looper.Rewinds(), 3);
        }

        /** LEVELS: part 0 the effects' meters; part 1 the VU meter, the compressor's reduction
         *  and the peaks since the last ask, the meters started by the first */
        MIDI_CONTROL_ONCE size_t DevLevels(uint8_t part, uint8_t* d, size_t n)
        {
            using namespace dev;
            if (part == 0)
            {
                for (size_t fx = 0; fx < kNumFx; fx++)
                    n = PutScaled(d, n, engine_->GetFxLevel(fx), 16383.f, 2);
                return n;
            }
            Audio& audio = midi_->audio;
            float peak[Audio::kPeaks];
            {
                ScopedIrqBlocker irq;
                memcpy(peak, audio.peak, sizeof(peak));
                memset(audio.peak, 0, sizeof(audio.peak));
            }
            d[n++] = audio.metering ? 0 : kArmed;
            audio.metering = true;
            audio.on = true;
            n = PutScaled(d, n, engine_->GetVUSample(), 16383.f, 2);
            n = PutScaled(d, n, -engine_->GetCompReduction(), 100.f, 2);
            for (float p : peak)
                n = PutScaled(d, n, p, 8192.f, 2); // full scale 8192, up to twice it
            return n;
        }

        /** SIGNAL WAVE FREQ(2) LEVEL(2): the test signal, or (0) the input again */
        MIDI_CONTROL_ONCE bool DevSignal(const MidiControl::DevQuery& q)
        {
            using namespace dev;
            Audio& audio = midi_->audio;
            const uint8_t wave = q.len ? q.arg[0] : Audio::kNumWaves;
            const uint16_t freq = q.len >= 5 ? static_cast<uint16_t>(q.arg[1] << 7 | q.arg[2]) : 0;
            if (wave == Audio::OFF)
            {
                audio.wave = Audio::OFF;
                audio.on = audio.metering;
                return true;
            }
            if (wave >= Audio::kNumWaves || !freq)
                return false;
            ScopedIrqBlocker irq;
            audio.inc = freq / 48000.f;
            audio.level = (q.arg[3] << 7 | q.arg[4]) / 16383.f;
            audio.phase = 0.f;
            audio.wave = wave;
            audio.on = true;
            return true;
        }

        /** A GET group's values, 14 bits each: how many, 0 for no such group */
        size_t DevValues(uint8_t group, uint16_t* v)
        {
            using namespace dev;
            if (group < kNumFx || group == kGroupComp)
            {
                for (size_t p = 0; p < kNumFxParams; p++)
                    v[p] = KnobToMidi14(group == kGroupComp ? fx_.CompParam(p) : fx_.Param(group, p));
                return kNumFxParams;
            }
            if (group == kGroupLatch)
            {
                for (size_t fx = 0; fx < kNumFx; fx++)
                    v[fx] = fx_.IsLatched(fx) ? 1 : 0;
                return kNumFx;
            }
            if (group == kGroupVolume)
            {
                const float gains[] = {out_gain_, settings_.InGain(), mix_, hp_cue_};
                for (size_t i = 0; i < 4; i++)
                    v[i] = KnobToMidi14(gains[i]);
                return 4;
            }
            if (group == kGroupSettings)
            {
                MasterSettings master;
                settings_.Store(master);
                size_t i = 0;
                for (const masterfile::Line& line : masterfile::kLines)
                    v[i++] = masterfile::Value(master, line);
                return i;
            }
            return 0;
        }

        /** A SET: as the knob, latch or setting would, the card written as theirs are (or
         *  not, after PERSIST 0). False for no such value, or one the setting doesn't take */
        bool DevSet(uint8_t group, uint8_t index, uint16_t raw)
        {
            using namespace dev;
            const float value = MidiToKnob(raw, true);
            if (group < kNumFx && index < kNumFxParams)
            {
                fx_.SetParamTo(group, index, value);
                HeardKey(WhiteKeyOfFx(group), index);
            }
            else if (group == kGroupComp && index < kNumFxParams)
            {
                fx_.SetComp(index, value);
                HeardKey(WhiteKeyOfFx(kNumFx), index);
            }
            else if (group == kGroupLatch && index < kNumFx && raw <= 1)
            {
                fx_.SetLatch(index, raw != 0);
                HeardKey(WhiteKeyOfFx(index), feedback::kLatchSlot);
            }
            else if (group == kGroupVolume && index < 4)
            {
                if (index == 0)
                    SetOutGain(value);
                else if (index == 1)
                {
                    SetInGain(value); // as VOLUME's page 2: to the card
                    MasterChanged(System::GetNow());
                }
                else if (index == 2)
                    SetMix(value);
                else
                    SetHpCue(value);
                Heard(static_cast<uint8_t>(feedback::kOutGain + index));
            }
            else if (group == kGroupSettings && settings_.Set(index, raw))
                MasterChanged(System::GetNow());
            else
                return false;
            return true;
        }

        /** zlib's CRC-32 of the image as loaded, which the host works out from the .bin
         *  (tools/builds.py): a nibble at a time, from a table of 16 */
        static uint32_t Crc32(const uint8_t* p, size_t len)
        {
            static const uint32_t kNibble[16] = {
                0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < len; i++)
            {
                crc ^= p[i];
                crc = (crc >> 4) ^ kNibble[crc & 15];
                crc = (crc >> 4) ^ kNibble[crc & 15];
            }
            return ~crc;
        }

        /** A scene's part for kCmdSceneGet: kFxPerPart effects' knobs on one page, 14 bits
         *  each (parts 0-3 page 1, 4-7 page 2), or (the last) whether it's used and its
         *  latches */
        static size_t ScenePart(const FxScene& scene, uint8_t part, uint8_t* d, size_t n)
        {
            if (part == kSceneParts - 1)
            {
                d[n++] = scene.used ? 1 : 0;
                return Put14(d, n, scene.latched);
            }
            const size_t first = part % kFxParts * kFxPerPart, page = part / kFxParts;
            for (size_t fx = first; fx < first + kFxPerPart; fx++)
                for (size_t k = 0; k < kNumFxKnobs; k++)
                    n = Put14(d, n, fx < kNumFx ? KnobToMidi14(scene.params[fx][page * kNumFxKnobs + k])
                                                : 0);
            return n;
        }

        /** A part of kCmdScenePut, into the scene being sent; the last part stores it in its
         *  slot and on the card, once every part has come. Never the blank scene */
        bool TakeScenePart(const MidiControl::Query& q)
        {
            if (q.a == kBlankSlot)
                return false;
            if (q.a != put_slot_)
            {
                put_slot_ = q.a;
                put_parts_ = 0;
            }
            const uint8_t* v = q.data;
            if (q.b == kSceneParts - 1)
            {
                if (q.len < 3 || put_parts_ != (1u << (kSceneParts - 1)) - 1)
                    return false;
                put_scene_.used = v[0] != 0;
                put_scene_.latched = static_cast<uint16_t>((v[1] << 7) | v[2]);
                scenes_->scenes[q.a] = put_scene_;
                put_slot_ = kNoScene;
                // the sound stays, so it no longer matches the active scene (as COPY onto it)
                if (q.a == scene_ctl_.Active())
                    fx_.MarkEdited();
                FlashWhenSaved(q.a);
                return true;
            }
            if (q.len < kFxPerPart * kNumFxKnobs * 2)
                return false;
            const size_t first = q.b % kFxParts * kFxPerPart, page = q.b / kFxParts;
            for (size_t i = 0; i < kFxPerPart * kNumFxKnobs && first + i / kNumFxKnobs < kNumFx; i++)
                put_scene_.params[first + i / kNumFxKnobs][page * kNumFxKnobs + i % kNumFxKnobs]
                    = MidiToKnob(static_cast<uint16_t>((v[2 * i] << 7) | v[2 * i + 1]), true);
            put_parts_ |= 1u << q.b;
            return true;
        }

        // PlayKeys' Host
        friend class PlayKeys<NormalPage>;
        inline bool SceneArmed() const { return scene_ctl_.Armed(); }
        /** SHIFT going down: SHIFT + key after all if an FX key's or the compressor's press
         *  came at most kShiftGraceMs before it, a select, which a plain press of an FX key
         *  takes back (FxControls::LateSelect); otherwise the FX keys held will latch */
        bool ShiftPressed()
        {
            const uint16_t shift = static_cast<uint16_t>(Hardware::SwId::KEY_26);
            bool select = Grace(static_cast<uint16_t>(kCompKey), true, shift);
            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                if (Grace(static_cast<uint16_t>(kFxSlots[fx].key), true, shift))
                {
                    fx_.LateSelect(fx);
                    select = true;
                }
            }
            if (!select)
                return fx_.ShiftPressed();
            fx_.ShiftUsed(); // as SHIFT, then the key, would leave the keys held long before
            select_flash_.Start(System::GetNow(), kSelectFlashMs);
            return true;
        }
        inline void ShiftUsed() { fx_.ShiftUsed(); }
        bool FreezeMorph()
        {
            ScopedIrqBlocker irq;
            return scene_ctl_.FreezeMorph();
        }
        bool DropScene()
        {
            ScopedIrqBlocker irq;
            return scene_ctl_.Drop();
        }
        /** SHIFT let go: a morph started with it glides now (FxMorph::Release), or the hand's
         *  crossfader ends where it is (SceneControls::EndFade). One on MIDI's crossfader
         *  (CC 80) stays MIDI's */
        void ReleaseMorph()
        {
            ScopedIrqBlocker irq;
            if (!(hand_fader_ && scene_ctl_.EndFade()))
                engine_->ReleaseFxMorph();
            hand_fader_ = false;
        }
        inline void Refused() { loop_refused_.Start(System::GetNow()); }
        inline uint32_t Now() const { return System::GetNow(); }
        inline Looper::State LooperState() const { return engine_->looper.GetState(); }
        inline bool CanRecordQuantized() { return engine_->looper.CanRecordQuantized(); }
        inline void StartRecording(bool quantized) { engine_->looper.StartRecording(quantized); }
        inline void StopRecording() { engine_->looper.StopRecording(); }
        inline void TogglePlay() { engine_->looper.TogglePlay(); }
        inline void Erase() { engine_->looper.Erase(); }
        inline void EraseAtEnd() { engine_->looper.EraseAtEnd(); }
        inline void CancelErase() { engine_->looper.CancelErase(); }
        inline bool ErasePending() const { return engine_->looper.IsErasePending(); }

        /** An FX or the compressor's key going down: tells PlayKeys, and flashes the
         *  key white for a select */
        void FxKeyDown(bool flash)
        {
            keys_.FxKey();
            if (flash)
                select_flash_.Start(System::GetNow(), kSelectFlashMs);
        }

        /** Once per frame, before drawing: what follows from time and the looper's state */
        void Update(uint32_t now)
        {
            // an erase starting, now or at the loop's end: the input fades in while the loop
            // fades out. A fade too short to be seen still ends in an empty looper.
            const Looper::State looper_state = engine_->looper.GetState();
            const bool erasing = engine_->looper.IsErasing()
                                 || (looper_state == Looper::State::EMPTY
                                     && (last_looper_state_ == Looper::State::PLAYING
                                         || last_looper_state_ == Looper::State::PAUSED));
            if (erasing && !last_erasing_)
            {
                SetMix(0.f);
                speed_chunk_ = 0.f;
            }
            last_erasing_ = erasing;

            // jump to fully wet when a recording closes into playback
            if (last_looper_state_ == Looper::State::RECORDING && looper_state == Looper::State::PLAYING)
            {
                SetMix(1.f);
                speed_chunk_ = 0.f; // a new loop starts at 1x with no detents carried over
            }
            last_looper_state_ = looper_state;

            // a scene confirmed: flash its slot once the card has been written, white if it was
            if (scene_flash_waiting_ && scenes_->GetSaveState() != SceneStore::SaveState::PENDING)
            {
                scene_flash_waiting_ = false;
                scene_flash_ok_ = scenes_->GetSaveState() == SceneStore::SaveState::OK;
                scene_flash_signal_.Start(now);
            }

            // the compressor's knobs to the card, once they've been left alone a while: those
            // the hand or SysEx set, not MIDI's CCs (#97 D6)
            if (const uint8_t comp = fx_.TakeCompChanged())
            {
                for (size_t p = 0; p < kNumFxParams; p++)
                    if ((comp >> p) & 1)
                        scenes_->master.comp[p] = fx_.CompParam(p);
                MasterChanged(now);
            }
            // a failed write: the compressor's key blinks red, and the settings page's key
            // changed last, and it's tried again a few times
            if (scenes_->TakeMasterFailed())
            {
                master_refused_.Start(now);
                settings_.SaveFailed(now);
                master_saving_ = false;
                if (++master_tries_ <= kMasterSaveTries)
                {
                    master_unsaved_ = true;
                    master_changed_at_ = now;
                }
            }
            else if (master_saving_ && !scenes_->Busy())
            {
                settings_.Saved();
                master_saving_ = false;
            }
            if (master_unsaved_ && now - master_changed_at_ > kMasterSaveDelayMs)
            {
                settings_.Store(scenes_->master);
                scenes_->RequestMasterSave();
                master_saving_ = true;
                master_unsaved_ = false;
            }
        }

        void VolumeTurned(float detents)
        {
            const float inc = detents * kVolumeStep;

            if (Shift())
                SetMix(mix_ + detents * kMixStep);
            else if (page_ == kOutGainPage)
                SetOutGain(out_gain_ + inc);
            else if (page_ == kInGainPage)
            {
                SetInGain(settings_.InGain() + inc);
                MasterChanged(System::GetNow());
            }
            else
                SetHpCue(hp_cue_ - detents * kHpCueStep); // left towards the dry input
        }

        /** A tempo tap: with a loop, it refits the loop's beats; without one, it sets the
         *  tempo, unless MIDI clock runs (TempoClock.h). now: when it was tapped */
        void Tap(uint32_t now = System::GetNow())
        {
            if (!engine_->CanTap())
            {
                Refused();
                return;
            }
            tap_flash_.Start(now, kTapFlashMs);
            if (tap_tempo_.Tap(now))
                engine_->TapTempo(tap_tempo_.Bpm());
        }

        /** The compressor's knobs or a setting changed: written kMasterSaveDelayMs after the last
         *  change, with every retry available again */
        void MasterChanged(uint32_t now)
        {
            master_unsaved_ = true;
            master_changed_at_ = now;
            master_tries_ = 0;
        }

        // VOLUME's pages and the mix, 0..1, to the engine
        void SetOutGain(float gain)
        {
            out_gain_ = fclamp(gain, 0.f, 1.f);
            engine_->SetMainGain(out_gain_);
        }
        /** The input gain of the source heard (SettingsPage::SetInGain), kept for the card unless
         *  MIDI's (keep false); its colour on VOLUME's LED for kGainShownMs, instead of the meter */
        void SetInGain(float gain, bool keep = true)
        {
            settings_.SetInGain(gain, keep);
            gain_turned_ = System::GetNow();
        }

        /** VOLUME's page on the settings page: 1 or 2, page 3 shown as 1 there */
        inline uint8_t SettingsVolumePage() const
        {
            return page_ == kInGainPage ? kInGainPage : kOutGainPage;
        }
        /** VOLUME turned on the settings page: page 1 or 2 as on the play page, SHIFT + turn
         *  nothing */
        void SettingsVolumeTurned(int16_t detents)
        {
            vol_touched_ = System::GetNow();
            if (Shift() || settings_.ShiftHeld())
                return;
            if (SettingsVolumePage() == kInGainPage)
            {
                SetInGain(settings_.InGain() + detents * kVolumeStep);
                MasterChanged(vol_touched_);
            }
            else
                SetOutGain(out_gain_ + detents * kVolumeStep);
        }
        /** VOLUME pressed on the settings page: on its release, page 1 or 2, unless it was SHIFT
         *  + press (the bug report's hold, SettingsPage::Held) */
        void SettingsVolumePressed(bool down, uint32_t now)
        {
            vol_touched_ = now;
            if (down)
            {
                vol_down_ = true;
                vol_shift_ = Shift() || settings_.ShiftHeld();
                return;
            }
            if (vol_down_ && !vol_shift_)
            {
                page_ = SettingsVolumePage() == kInGainPage ? kOutGainPage : kInGainPage;
                page_flash_.Start(now, (page_ + 1) * 2 * kSignalBlinkMs);
            }
            vol_down_ = false;
        }

        /** The input's meter while page 2 shows it: the engine's peaks, falling back slowly */
        void MeterInput(uint32_t now, bool on)
        {
            engine_->MeterInput(on);
            const float peak = engine_->TakeInputPeak();
            if (!on)
            {
                in_meter_ = 0.f;
                return;
            }
            in_meter_ = fmaxf(peak, in_meter_ * kInMeterFall);
            if (peak >= kLoopCeiling)
                in_red_at_ = now;
        }

        /** VOLUME's LED on a page: the page's number blinking after a press, the output's VU
         *  meter as bright as the output gain, the input's meter (the gain after a turn), the
         *  headphone feed */
        void DrawVolumeLed(uint32_t now, uint8_t page)
        {
            float rgb[3];
            if (page_flash_.Active(now))
            {
                // a page picked: blinks its number in white
                rgb[0] = rgb[1] = rgb[2] = page_flash_.BlinkLit(now) ? 1.f : 0.f;
            }
            else if (page == kOutGainPage)
            {
                Xfade4(kVuDim, green, yellow, pink, engine_->GetVUSample(), rgb);
                for (float& c : rgb)
                    c *= out_gain_;
            }
            else if (page == kInGainPage)
            {
                if (now - gain_turned_ < kGainShownMs)
                    Xfade(blue, red, settings_.InGain(), rgb);
                else if (now - in_red_at_ < kInRedMs)
                    Xfade(red, red, 1.f, rgb); // the loop would clip here, without its limit
                else
                {
                    const float db = 20.f * log10f(fmaxf(in_meter_ / kLoopCeiling, 1e-6f));
                    Xfade3(kVuDim, green, yellow, fclamp(1.f - db / kInMeterFloorDb, 0.f, 1.f), rgb);
                }
            }
            else
                Xfade(white, green, hp_cue_, rgb);
            SetPthLedFloat(kVolumeLed, rgb[0], rgb[1], rgb[2]);
        }
        void SetHpCue(float cue)
        {
            hp_cue_ = fclamp(cue, 0.f, 1.f);
            engine_->SetHeadphoneCue(hp_cue_);
        }
        void SetMix(float mix)
        {
            mix_ = fclamp(mix, 0.f, 1.f);
            engine_->SetMix(mix_);
        }

        /** A scene key, the hand's or MIDI's: a new morph runs over bars bar lines. A refusal
         *  blinks the slot */
        SceneControls<PassthroughEngine>::Slot ScenePressed(size_t slot, bool shift,
                                                            uint8_t bars = 1)
        {
            SceneControls<PassthroughEngine>::Slot result;
            {
                // a recall within one audio block
                ScopedIrqBlocker irq;
                result = scene_ctl_.Press(slot, shift);
                if (result == SceneControls<PassthroughEngine>::Slot::MORPH)
                {
                    hand_fader_ = false; // a new morph: no fader has it yet
                    for (uint8_t bar = 1; bar < bars; bar++)
                        scene_ctl_.MorphMore();
                }
            }
            if (result == SceneControls<PassthroughEngine>::Slot::REFUSED)
                SceneRefusedBlink(slot);
            return result;
        }

        void ConfirmScene()
        {
            const int slot = scene_ctl_.Confirm();
            if (slot != kNoScene)
                FlashWhenSaved(slot);
        }

        /** A slot changed: to the card, and it flashes once SceneStore::Process has written
         *  it (Update) */
        void FlashWhenSaved(int slot)
        {
            scenes_->RequestSave();
            scene_flash_ = slot;
            scene_flash_waiting_ = true;
            scene_flash_signal_.Stop();
        }

        void SceneRefusedBlink(size_t slot)
        {
            scene_refused_ = static_cast<int>(slot);
            scene_refused_signal_.Start(System::GetNow());
        }

        /** The current scene mode's colour, white without one */
        const float* SceneModeColor() const
        {
            for (const SceneModeKey& key : kSceneModeKeys)
            {
                if (scene_ctl_.Mode() == key.mode)
                    return key.color;
            }
            return white;
        }

        void DrawSceneLeds(uint32_t now)
        {
            const SceneMode mode = scene_ctl_.Mode();
            const float* mode_color = SceneModeColor();
            for (const SceneModeKey& key : kSceneModeKeys)
                SmtLed(key.led, key.color, mode == key.mode ? 1.f : kFxOffLevel);

            const bool blink_on = BlinkOn(now, kPendingBlinkMs);
            for (size_t slot = 0; slot < kNumSlots; slot++)
            {
                const int s = static_cast<int>(slot);
                const float* color = white;
                float level = 0.f;
                if (s == scene_flash_ && scene_flash_waiting_)
                    level = 1.f; // writing the card
                else if (s == scene_flash_ && scene_flash_signal_.Active(now))
                {
                    color = scene_flash_ok_ ? white : red;
                    level = scene_flash_signal_.BlinkLit(now) ? 1.f : 0.f;
                }
                else if (s == scene_refused_ && scene_refused_signal_.Active(now))
                {
                    color = red;
                    level = scene_refused_signal_.BlinkLit(now) ? 1.f : 0.f;
                }
                else if (mode != SceneMode::NONE)
                {
                    // in a mode: the source lit, the pick blinking, the slots it can act on
                    // dim, all in the mode's colour; the rest dark
                    color = mode_color;
                    if (s == scene_ctl_.Source())
                        level = 1.f;
                    else if (s == scene_ctl_.Selected())
                        level = blink_on ? 1.f : 0.f;
                    else if (scene_ctl_.Valid(slot))
                        level = kFxOffLevel;
                }
                else if (scene_ctl_.Fading()
                         && (s == scene_ctl_.Morphing() || s == scene_ctl_.FadeFrom()))
                {
                    // the crossfader: where it started and where it goes, in their mix
                    const float t = scene_ctl_.FaderPos();
                    float mix = s == scene_ctl_.Morphing() ? t : 0.f;
                    if (s == scene_ctl_.FadeFrom())
                        mix = fmaxf(mix, 1.f - t);
                    level = kFxOffLevel + (1.f - kFxOffLevel) * mix;
                }
                else if (s == scene_ctl_.Morphing())
                {
                    // morphing to it: blinking on the FX clock's beats; red for a drop
                    if (scene_ctl_.Dropping())
                        color = red;
                    const uint32_t pos = engine_->FxClockPosition();
                    level = pos % kPulsesPerBeat < kPulsesPerBeat / 2 ? 1.f : 0.f;
                }
                else if (s == scene_ctl_.Active())
                {
                    // edited: a slow pulse between the saved and the active brightness
                    const float phase = static_cast<float>(now % kScenePulseMs) / kScenePulseMs;
                    level = scene_ctl_.Edited() ? .6f + .4f * cosf(phase * TWOPI_F) : 1.f;
                }
                else if (scenes_->scenes[slot].used)
                    level = kFxOffLevel;
                SmtLed(kSceneKeys[slot].led, color, level);
            }
        }

        void DrawFxLeds(uint32_t now)
        {
            // knob LEDs: the selected FX's parameters in its colours, or the compressor's; a
            // knob the page doesn't use is dark. On an effect's page 2, knob 4's is its output's
            // VU meter (#117); a page just turned blinks its number on all four, white
            const size_t selected = fx_.Selected();
            const bool comp = selected == kCompSelected;
            const float* const* colors = comp ? kCompKnobColors : kFxSlots[selected].knob_colors;
            // the page it blinks for, so another effect's page 1 isn't blinked over
            const bool flash = knob_page_flash_.Active(now) && fx_.Page() == knob_flash_page_;
            const float lit = knob_page_flash_.BlinkLit(now) ? 1.f : 0.f;
            const FxParams& knobs = fx_.Knobs();
            for (size_t k = 0; k < kNumFxKnobs; k++)
            {
                float rgb[3] = {0.f, 0.f, 0.f};
                if (flash)
                    rgb[0] = rgb[1] = rgb[2] = lit;
                else if (fx_.KnobUsed(k) && fx_.Page() && !comp && k == kVuKnob)
                    FxVu(selected, rgb);
                else if (fx_.KnobUsed(k))
                    KnobColor(knobs, fx_.ParamOf(k), fx_.Knob(k), colors, rgb);
                SetPthLedFloat(kFxKnobLeds[k], rgb[0], rgb[1], rgb[2]);
            }

            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                const float* color = kFxSlots[fx].key_color;
                const float meter = FxMeter(fx);
                float level = kFxOffLevel;
                float whiten = 0.f; // how far towards white
                // dropped from this step by the chaos key: dim, as if off
                if (fx_.IsOn(fx) && !(engine_->FxDropped() >> fx & 1))
                {
                    level = 1.f;
                    // squared, so normal levels stay coloured and the peaks flash white
                    whiten = kFxWhiteMax * meter * meter;
                }
                else if (kFxSlots[fx].kind == FxKind::SEND)
                {
                    // a send's tail ringing out, from full down to off, in even steps to the eye
                    level = kFxOffLevel * powf(1.f / kFxOffLevel, meter);
                }
                float rgb[3];
                for (int c = 0; c < 3; c++)
                    rgb[c] = level * (color[c] + whiten * (1.f - color[c]));
                // a select (SHIFT + the key): a flash
                if (fx == selected && select_flash_.Active(now))
                    Flash(rgb);
                SetSmtLedFloat(kFxSlots[fx].key_led, rgb[0], rgb[1], rgb[2]);
            }

            // the compressor's key: its gain reduction, from dim up to full, white; turning red
            // while the safety limiter works, the one thing that sets the level by itself; a
            // select flashes
            const float reduced = -engine_->GetCompReduction() / kCompMeterDb;
            const float level = kFxOffLevel + (1.f - kFxOffLevel) * fclamp(reduced, 0.f, 1.f);
            const float lim_db = -20.f * log10f(fmaxf(engine_->TakeLimiterGain(), 1e-6f));
            const float lim = fclamp((lim_db - kLimStartDb) / (kLimFullDb - kLimStartDb), 0.f, 1.f);
            if (lim >= lim_shown_ || now - lim_at_ > kLimHoldMs)
            {
                lim_shown_ = lim;
                lim_at_ = now;
            }
            float rgb[3] = {level, level, level};
            for (int c = 0; c < 3; c++)
                rgb[c] += lim_shown_ * (red[c] - rgb[c]);
            if (comp && select_flash_.Active(now))
                Flash(rgb);
            if (master_refused_.Active(now))
                SmtLed(kCompKeyLed, red, master_refused_.BlinkLit(now) ? 1.f : 0.f);
            else
                SetSmtLedFloat(kCompKeyLed, rgb[0], rgb[1], rgb[2]);
        }

        /** fx's meter (about its output's amplitude, a send's return) on a dB scale: 0 at
         *  kFxMeterFloorDb, 1 at 0 dBFS */
        float FxMeter(size_t fx)
        {
            const float db = 20.f * log10f(fmaxf(engine_->GetFxLevel(fx), 1e-6f));
            return fclamp(1.f - db / kFxMeterFloorDb, 0.f, 1.f);
        }

        /** Page 2's knob 4 of an effect: its output's VU meter, in VOLUME's colours, as bright
         *  as its Level turns it, as VOLUME's is on its page 1. An effect that's off (or dropped
         *  by the chaos key) shows nothing but the floor; a send shows its return, tails too */
        void FxVu(size_t fx, float* rgb)
        {
            const bool send = kFxSlots[fx].kind == FxKind::SEND;
            const bool sounds = send || (fx_.IsOn(fx) && !(engine_->FxDropped() >> fx & 1));
            Xfade4(kVuDim, green, yellow, pink, sounds ? FxMeter(fx) : 0.f, rgb);
            for (int c = 0; c < 3; c++)
                rgb[c] *= fx_.Knob(kVuKnob);
        }

        void DrawLooperLeds(uint32_t now)
        {
            const Looper& looper = engine_->looper;
            const Looper::State state = looper.GetState();

            // the beat the effects follow (TempoClock.h), as the settings page's G# shows it:
            // when one began, for the flash, and whether it was a bar's 1
            const uint32_t pulse = engine_->FxClockPosition();
            if (pulse / kPulsesPerBeat != beat_)
            {
                beat_ = pulse / kPulsesPerBeat;
                beat_at_ = now;
                downbeat_ = pulse % kPulsesPerBar < kPulsesPerBeat;
            }
            const float beat_half = pulse % kPulsesPerBeat < kPulsesPerBeat / 2 ? kBeatPulse : 0.f;

            float play = 0.f;  // PLAY LED, white level
            float loop[3] = {0.f, 0.f, 0.f};

            if (state == Looper::State::RECORDING)
            {
                const bool on = !looper.IsClosing() || BlinkOn(now, kPendingBlinkMs);
                loop[0] = on ? 1.f : 0.f;
            }
            else if (LoopExists())
            {
                const float level = state == Looper::State::PLAYING ? 1.f : kPausedDim;
                const float pos = looper.GetPosition();
                play = (1.f - pos) * level;
                loop[0] = loop[1] = loop[2] = pos * level;
                // an erase waiting for the loop's end: LOOP blinks red, as a closing record
                if (looper.IsErasePending())
                {
                    loop[0] = BlinkOn(now, kPendingBlinkMs) ? 1.f : 0.f;
                    loop[1] = loop[2] = 0.f;
                }
            }

            // the beat on PLAY (#110): playing, a flash on each beat over where the loop is;
            // otherwise its first half, a count for a recording and the tempo for the next
            // the flash, as a tap's (Flash): whiter, or darker on a PLAY already this white,
            // so the bar's 1 shows at the loop's start too
            if (state == Looper::State::PLAYING)
            {
                if (now - beat_at_ < kBeatFlashMs)
                {
                    const float flash = downbeat_ ? kDownbeatFlash : kBeatFlash;
                    play = play > kFlashDarkAbove ? play - flash : play + flash;
                }
            }
            else
                play += beat_half;
            play = play > 1.f ? 1.f : (play < 0.f ? 0.f : play);

            // a tempo tap: a flash, over whatever LOOP was showing
            if (tap_flash_.Active(now))
                Flash(loop);

            // refused quantized record or tap: 3 red blinks, over whatever LOOP was showing
            if (loop_refused_.Active(now))
            {
                loop[0] = loop_refused_.BlinkLit(now) ? 1.f : 0.f;
                loop[1] = loop[2] = 0.f;
            }

            SetPthLedFloat(kPlayLed, play, play, play);
            SetPthLedFloat(kLoopLed, loop[0], loop[1], loop[2]);

            // the crossfader in the hand: where it is, in white, instead of the speed: the
            // start on the reverse LED, the scene on the forward one
            if (scene_ctl_.Fading())
            {
                const float t = scene_ctl_.FaderPos();
                SetPthLedFloat(kTransportLedRev, 1.f - t, 1.f - t, 1.f - t);
                SetPthLedFloat(kTransportLedFwd, t, t, t);
            }
            else if (state == Looper::State::PLAYING)
                DrawSpeedLeds(looper.GetSpeed());
            else if (state == Looper::State::PAUSED)
            {
                // scrub speed in white on the LED for its direction
                const float scrub = looper.GetScrub() * .5f;
                const float level = fabsf(scrub);
                SetPthLedFloat(scrub > 0.f ? kTransportLedFwd : kTransportLedRev, level, level, level);
            }
        }

        /** The event log (SHIFT + VOLUME held on the settings page): both transport LEDs blink
         *  white while it's written, then 3 blinks: white when it's on the card, red when it
         *  isn't; on either page */
        void DrawLogLeds(uint32_t now)
        {
            if (log_->Written() != log_written_ || log_->Failed() != log_failed_)
            {
                log_ok_ = log_->Written() != log_written_;
                log_written_ = log_->Written();
                log_failed_ = log_->Failed();
                log_signal_.Start(now);
            }
            float level = -1.f;
            if (log_->Writing())
                level = BlinkOn(now, kPendingBlinkMs) ? 1.f : 0.f;
            else if (log_signal_.Active(now))
                level = log_signal_.BlinkLit(now) ? 1.f : 0.f;
            if (level < 0.f)
                return;
            const float* color = log_->Writing() || log_ok_ ? white : red;
            PthLed(kTransportLedRev, color, level);
            PthLed(kTransportLedFwd, color, level);
        }

        /** TAPE's transport colours: speed -2..2 maps to 0..1; blue at the extremes through
         *  green and yellow to red towards a stop. The LED for the direction is lit, and the
         *  other one glows red as the speed nears zero. */
        void DrawSpeedLeds(float speed)
        {
            const float value = speed * .25f + .5f;
            const float idx = value < .5f ? value * 2.f : (1.f - value) * 2.f; // 0 - 1 - 0
            const uint8_t led_on = value > .5f ? kTransportLedFwd : kTransportLedRev;
            const uint8_t led_off = value > .5f ? kTransportLedRev : kTransportLedFwd;

            float rgb[3];
            Xfade4(med_blue, green, yellow, red, idx, rgb);
            SetPthLedFloat(led_on, rgb[0], rgb[1], rgb[2]);

            if (idx > .8f)
                PthLed(led_off, red, (idx - .8f) * 5.f);
        }

        /** Playing: a turn steps the speed in semitones, SHIFT + turn along the ladder (see
         *  Looper::StepSpeed). Paused: a turn scrubs, SHIFT + turn does nothing. SHIFT + turn
         *  while SHIFT holds a morph: the crossfader, loop or not */
        void TransportTurned(int16_t turns)
        {
            // SHIFT still holding a morph, or the crossfader already in the hand: the
            // crossfader, with or without a loop
            if (Shift())
            {
                bool fader;
                {
                    ScopedIrqBlocker irq;
                    fader = scene_ctl_.FaderTurned(turns);
                }
                if (fader)
                {
                    hand_fader_ = true;
                    keys_.Used();
                    return;
                }
            }
            if (!LoopExists())
                return;

            Looper& looper = engine_->looper;
            const bool ladder = Shift();
            if (looper.GetState() == Looper::State::PAUSED)
            {
                if (!ladder)
                    looper.Scrub(turns);
                return;
            }

            keys_.Used();
            if (ladder != speed_ladder_)
                speed_chunk_ = 0.f; // detents don't carry over from one kind of step to the other
            speed_ladder_ = ladder;
            speed_chunk_ += turns * (ladder ? kSpeedStepPerTurn : kSemiStepPerTurn);
            if (speed_chunk_ >= 1.f || speed_chunk_ <= -1.f)
            {
                const int dir = speed_chunk_ > 0.f ? 1 : -1;
                if (ladder)
                    looper.StepSpeed(dir);
                else
                    looper.StepSemitone(dir);
                speed_chunk_ = 0.f;
            }
        }

        inline bool LoopExists() const
        {
            const Looper::State state = engine_->looper.GetState();
            return state == Looper::State::PLAYING || state == Looper::State::PAUSED;
        }

        inline bool Shift() const { return keys_.Shift(); }
        /** Whether key's last press (down) or release went at most kShiftGraceMs before
         *  the last press of key `at`, by the audio callback's clock (KeyEdge). Not if it went
         *  after: then their events came in late, together, and keep their order */
        inline bool Grace(uint16_t key, bool down, uint16_t at) const
        {
            return edge_ms_[1][at] - edge_ms_[down][key] <= kShiftGraceMs;
        }
        /** SHIFT for an FX key's or the compressor's press: held, or let go at most
         *  kShiftGraceMs before it */
        inline bool ShiftAt(uint16_t key) const
        {
            return Shift() || Grace(static_cast<uint16_t>(Hardware::SwId::KEY_26), false, key);
        }

        // LED helpers (a colour at a level: PanelLeds.h): crossfades through two, three or
        // four colours
        /** A short flash over what an LED shows: white, or dark where it's already close to
         *  white (a loud FX, the compressor working hard, LOOP near the loop's end), so it's
         *  always seen */
        static void Flash(float* rgb)
        {
            const float least = fminf(rgb[0], fminf(rgb[1], rgb[2]));
            const float flash = least > kFlashDarkAbove ? 0.f : 1.f;
            rgb[0] = rgb[1] = rgb[2] = flash;
        }
        static void Xfade(const float* a, const float* b, float t, float* rgb)
        {
            for (int c = 0; c < 3; c++)
                rgb[c] = color_xfade(a[c], b[c], t);
        }
        /** A knob's LED. White only at a neutral point: a bipolar knob (FxParams::bipolar) is
         *  white at its default, blue below and orange above, the same on every effect; a
         *  split one (FxParams::split) goes from the effect's first colour to its last in its
         *  lower half, and green to yellow in its upper; any other goes from the effect's
         *  first colour to its last */
        static void KnobColor(const FxParams& knobs, size_t param, float val,
                              const float* const* colors, float* rgb)
        {
            if ((knobs.bipolar >> param) & 1)
            {
                const float n = knobs.defaults[param];
                const float t = val <= n ? (n > 0.f ? .5f * val / n : .5f)
                                         : .5f + .5f * (val - n) / (1.f - n);
                Xfade3(kBipolarLow, white, kBipolarHigh, t, rgb);
                return;
            }
            if ((knobs.split >> param) & 1)
            {
                if (SplitUpper(val))
                {
                    const float t = static_cast<float>(SplitStep(val, knobs.steps[param]))
                                    / static_cast<float>(knobs.steps[param] - 1);
                    for (int i = 0; i < 3; i++)
                        rgb[i] = kSplitLow[i] + t * (kSplitHigh[i] - kSplitLow[i]);
                    return;
                }
                val = SplitLower(val);
            }
            for (int i = 0; i < 3; i++)
                rgb[i] = colors[0][i] + val * (colors[2][i] - colors[0][i]);
        }

        static void Xfade3(const float* a, const float* b, const float* c, float t, float* rgb)
        {
            for (int i = 0; i < 3; i++)
                rgb[i] = color_triple_xfade(a[i], b[i], c[i], t);
        }
        static void Xfade4(const float* a, const float* b, const float* c, const float* d,
                           float t, float* rgb)
        {
            for (int i = 0; i < 3; i++)
                rgb[i] = color_quad_xfade(a[i], b[i], c[i], d[i], t);
        }

        Hardware *hw_;
        PassthroughEngine *engine_;
        SceneStore *scenes_;
        EventLog *log_;
        MidiControl *midi_ = nullptr;
        uint8_t told_focus_ = 0;       // the white key MIDI's focus was told (Remote)
        MidiFeedback feedback_;        // MIDI feedback's sweep (MidiFeedback.h)
        bool feedback_on_ = false;     // it's on, and MIDI out
        uint8_t feedback_usb_tries_ = 0;
        static const uint8_t kFeedbackUsbTries = 5;
        FxScene put_scene_;            // a scene coming over SysEx (kCmdScenePut),
        int put_slot_ = kNoScene;      // for this slot,
        uint32_t put_parts_ = 0;       // with these parts so far
        uint32_t log_written_ = 0, log_failed_ = 0; // the log's files shown so far
        bool log_ok_ = false;
        LedSignal log_signal_; // the last file written, or not

        Looper::State last_looper_state_ = Looper::State::EMPTY;
        bool last_erasing_ = false;

        float out_gain_;
        float mix_;
        float hp_cue_; // headphones: 0 = the master's mirror, 1 = the dry input
        uint8_t page_;

        PlayKeys<NormalPage> keys_;
        LedSignal loop_refused_; // a refused quantized record or tap
        TapTempo tap_tempo_;
        LedSignal tap_flash_;
        uint32_t beat_ = 0;         // the beat the PLAY LED shows (DrawLooperLeds), in pulses / beat
        uint32_t beat_at_ = 0;      // when it began
        bool downbeat_ = false;     // it's a bar's 1
        LedSignal select_flash_; // on the selected FX's key
        // when each key of this page last went down ([1]) and up ([0]), by the audio
        // callback's clock (KeyEdge), for the grace of kShiftGraceMs
        static const int kEdgeKeys = static_cast<int>(Hardware::SwId::SR_LAST);
        volatile uint32_t edge_ms_[2][kEdgeKeys] = {};
        float lim_shown_ = 0.f;  // the limiter on the compressor's key, 0..1, held from lim_at_
        uint32_t lim_at_ = 0;
        float speed_chunk_ = 0.f;   // transport detents towards the next speed step
        bool speed_ladder_ = false; // speed_chunk_ counts towards a step of the ladder (SHIFT)
        SettingsPage settings_;
        volatile bool show_settings_ = false; // the mode switch is up (ui.h)
        bool drew_settings_ = false;          // the last frame was the settings page's
        bool master_unsaved_ = false;     // the compressor's knobs or a setting,
                                          // not yet on the card
        uint32_t master_changed_at_ = 0;  // when they last changed
        bool master_hurried_ = false;     // a restart pulled the save forward (MasterSettled)
        uint32_t master_tries_ = 0;       // failed writes since
        bool master_saving_ = false;      // a write asked for, its outcome not yet seen
        LedSignal master_refused_;        // a failed write, on the compressor's key

        FxControls<PassthroughEngine> fx_;
        SceneControls<PassthroughEngine> scene_ctl_;
        int scene_flash_ = kNoScene;   // confirmed, flashing
        LedSignal scene_flash_signal_;
        bool scene_flash_waiting_ = false; // for the card to be written
        bool scene_flash_ok_ = false;  // saved to the card
        int scene_refused_ = kNoScene; // refused, pressed
        LedSignal scene_refused_signal_;

        bool vol_down_ = false;     // VOLUME's press was seen here, not by the boot page
        bool vol_shift_ = false;    // VOLUME's press was SHIFT + press
        uint32_t vol_touched_ = 0;  // VOLUME last turned or pressed on the settings page
        uint32_t gain_turned_ = 0;  // the input gain last set
        float in_meter_ = 0.f;      // the input's meter, full scale 1
        uint32_t in_red_at_ = 0;    // the input last reached the loop's ceiling
        // CC 84's value for each input, PassthroughEngine::Input's order (MidiControl.h)
        static constexpr uint8_t kSourceTo7[3] = {0, 64, 127};
        bool hand_fader_ = false;   // the transport knob has had the crossfader since SHIFT
                                    // went down (TransportTurned)
        LedSignal page_flash_;      // a page picked: its number in blinks
        LedSignal knob_page_flash_; // the FX knobs' page turned: its number in blinks
        size_t knob_flash_page_ = 0; // the page knob_page_flash_ blinks for
    };

    constexpr uint8_t NormalPage::kSourceTo7[];

} // namespace chompi

#pragma GCC pop_options
