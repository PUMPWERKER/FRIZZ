/** @file NormalPage.h
 *  @brief The main play-mode UiPage (see ui.h): VOLUME, the looper's keys and transport, the
 *  punch-in FX keys and knobs (FxControls.h, FxSlots.h) and the FX scenes (SceneControls.h,
 *  SceneStore.h), with their LEDs. The logic is in FxControls.h and SceneControls.h; this
 *  routes the hardware to it and draws. MANUAL.md describes every control; what's here is what the manual doesn't say.
 *
 *  SHIFT is the CHOMPI key held. VOLUME's page 3 sets the headphone feed, from the master out
 *  to the dry input (passthroughEngine.h). The mode switch up shows the settings page instead
 *  (SettingsPage.h, ShowSettings): the hand's keys go there, while MIDI's still come here,
 *  and its settings go to the master file as the compressor's knobs do.
 *
 *  The CHOMPI, PLAY and LOOP keys' rules are in PlayKeys.h: SHIFT, the confirm tap in a scene
 *  mode, the looper's combos, tap tempo (TapTempo.h). This page is its Host. Every other key
 *  or knob used tells it (Used, FxKey), so SHIFT + it is a combo rather than a confirm.
 *
 *  FX keys: an FX key held, then SHIFT, toggles the latch, decided on the key's release;
 *  SHIFT, then an FX key, only selects it for the knobs, silently (FxControls.h), and the key
 *  flashes white. Knobs 1-4 edit the FX pressed or selected last.
 *
 *  The master compressor's key (kCompKey) selects it for the knobs, and flashes white; it's
 *  always on, so the key does nothing else. Its knobs go to the card (SceneStore's master
 *  file) kMasterSaveDelayMs after the last turn, so a sweep is one write.
 *
 *
 *  Scenes: a recall sends only the parameters that change, within one audio block and at the
 *  fast slew (FxChain::FastSlew), so an FX the two scenes share runs on untouched. SHIFT +
 *  a scene key morphs to it instead (FxMorph.h), landing on a bar line; the engine times it.
 *  SHIFT + PLAY stops a morph where it is; without one, PLAY works as ever. The transport knob
 *  turned while SHIFT still holds a morph is its crossfader (#66, SceneControls::FaderTurned):
 *  the glide in the hand, until SHIFT is let go (ReleaseMorph); the transport LEDs show it.
 *  A morph on MIDI's crossfader (CC 118) stays MIDI's: letting go of SHIFT doesn't end it.
 *  The card is written from MainLoop (SceneStore::Process), never here.
 *
 *  MIDI (MidiControl.h): notes and FRIZZ's SysEx keys and detents come as the hand's do (ui.h);
 *  the rest, Remote() takes from MainLoop: a controller sets a parameter, a latch, a gain or
 *  the mix outright, as its knob or key would, a program change is a scene key, a morph over
 *  MIDI glides at once over the bars set. It answers SysEx queries over USB.
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
    static const float kDefaultInGain = .75f;
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
    // waiting for something: a quantized record closing, an erase at the loop's end, a picked
    // scene slot, the CHOMPI key armed to confirm. Slower than a refusal's 3 blinks (LedSignal)
    static const uint32_t kPendingBlinkMs = 250;
    static const uint32_t kTapFlashMs = 80;      // LOOP flashes on a tempo tap
    static const uint32_t kSelectFlashMs = 80;   // an FX key flashes on a select
    static const float kFlashDarkAbove = .5f;    // a flash goes dark on an LED this white (Flash)
    static const float kSemiStepPerTurn = .5f;   // 2 transport detents per semitone
    static const float kSpeedStepPerTurn = .25f; // SHIFT: 4 detents per step of the ladder

    static const uint8_t kFxKnobLeds[kNumFxKnobs] = {1, 2, 3, 4}; // PTH LEDs of knobs 1-4
    // Page 2 of the FX knobs: their LEDs pulse, from full down to kPage2Low and back, faster
    // than an edited scene's key, so turning a knob never surprises
    static const uint32_t kPage2PulseMs = 600;
    static const float kPage2Low = .15f;
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
    // The safety limiter on the compressor's key: red from kLimStartDb of limiting, fully red
    // at kLimFullDb, held kLimHoldMs so a short peak is seen
    static constexpr float kLimStartDb = 1.f, kLimFullDb = 3.f;
    static const uint32_t kLimHoldMs = 300;
    static const uint32_t kMasterSaveDelayMs = 2000;
    static const uint32_t kMasterSaveTries = 3; // a failed write is tried again, this often

    // FX scenes: the slots on KEY_16-20, the lower octave's dark keys, the blank one first;
    // SAVE / COPY / DELETE on TAPE's preset keys in TEMPO's colours. One language for all
    // three: the mode's colour shows what will happen (the slots it can act on, the pick, the
    // CHOMPI key that confirms), white that it's done, red that it was refused or isn't on
    // the card. So no mode is red: DELETE is amber
    struct SceneKey
    {
        Hardware::SwId key;
        uint8_t led;
    };
    static const SceneKey kSceneKeys[kNumSlots] = {
        {Hardware::SwId::KEY_16, 0},
        {Hardware::SwId::KEY_17, 1},
        {Hardware::SwId::KEY_18, 2},
        {Hardware::SwId::KEY_19, 3},
        {Hardware::SwId::KEY_20, 4},
    };
    struct SceneModeKey
    {
        SceneMode mode;
        Hardware::SwId key;
        uint8_t led;
        const float* color;
    };
    static const SceneModeKey kSceneModeKeys[] = {
        {SceneMode::SAVE, Hardware::SwId::KEY_25, 9, blue},
        {SceneMode::COPY, Hardware::SwId::KEY_24, 8, green},
        {SceneMode::DELETE, Hardware::SwId::KEY_23, 7, amber}, // red means refused
    };
    static const uint32_t kScenePulseMs = 1000;     // the active scene, edited

    static const float kVuDim[3] = {.1f, .1f, .1f}; // the VU meter's floor

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
            SetInGain(kDefaultInGain);
            SetMix(kDefaultMix);
            SetHpCue(0.f); // the headphones mirror the master at power-on

            fx_.Init(engine_);
            // the compressor's knobs as they were left, from the card
            for (size_t p = 0; p < kNumFxParams; p++)
                fx_.SetComp(p, scenes_->master.comp[p]);
            fx_.TakeCompChanged();
            // and the settings page's: the mono input, the clock's factor, the LEDs
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
                settings_.Draw();
                DrawLogLeds(now);
                fill_led_data();
                return;
            }

            float rgb[3];

            if (Shift())
                Xfade(green, purple, mix_, rgb);
            else if (page_flash_.Active(now))
            {
                // a page picked: blinks its number in white
                rgb[0] = rgb[1] = rgb[2] = page_flash_.BlinkLit(now) ? 1.f : 0.f;
            }
            else if (page_ == kOutGainPage)
            {
                Xfade4(kVuDim, green, yellow, pink, engine_->GetVUSample(), rgb);
                for (float& c : rgb)
                    c *= out_gain_;
            }
            else if (page_ == kInGainPage)
                Xfade(blue, red, in_gain_, rgb);
            else
                Xfade(white, green, hp_cue_, rgb);
            SetPthLedFloat(kVolumeLed, rgb[0], rgb[1], rgb[2]);

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
                settings_.Held(key, rising, System::GetNow());
                if (rising && settings_.Key(key))
                    MasterChanged(System::GetNow());
                return true;
            }
            switch (buttonID)
            {
            case static_cast<uint16_t>(Hardware::SwId::KEY_26):
            {
                // a release whose press the boot page had lets go of nothing
                const bool held = Shift();
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
                    fx_.CompKeyPressed(Shift());
                }
                return true;
            }
            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                if (buttonID == static_cast<uint16_t>(kFxSlots[fx].key))
                {
                    if (rising)
                        FxKeyDown(Shift());
                    fx_.KeyPressed(fx, rising, Shift());
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
                    if (fx_.KnobPressed(knob, Shift()))
                        keys_.Used();
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
            }
            else if (encoderID == kVolumeEncoder)
            {
                keys_.Used();
                VolumeTurned(turns);
            }
            else
                return false;
            return true;
        }

        /** What came over MIDI for the page, from MainLoop: controllers, a program change, the
         *  transport, settings to keep and a query to answer */
        MIDI_CONTROL_ONCE void Remote()
        {
            const uint32_t now = System::GetNow();
            uint16_t cc;
            uint8_t raw;
            float value;
            while (midi_->TakeValue(cc, value, raw))
                RemoteValue(cc, value, raw);

            // a program change is its scene key, pressed
            const int program = midi_->TakeProgram();
            if (program >= 0)
                ScenePressed(static_cast<size_t>(program), false);

            // MIDI Start / Continue plays the loop, Stop pauses it
            const int transport = midi_->TakeTransport();
            if (transport && LoopExists()
                && (transport > 0) != (LooperState() == Looper::State::PLAYING))
                engine_->looper.TogglePlay();

            if (midi_->TakeSettingsChanged())
                MasterChanged(now);

            MidiControl::Query query;
            if (midi_->TakeQuery(query))
                Answer(query);
        }

    private:
        MIDI_CONTROL_ONCE void RemoteValue(uint16_t cc, float value, uint8_t raw)
        {
            using namespace midimap;
            // page 1's from the CC, page 2's from its NRPN in bank 1 (MidiControl.h)
            const size_t bank = cc / 128, ctl = cc % 128;
            if (ctl >= kParamCC && ctl < kParamCC + kMidiFx * kNumFxKnobs)
                fx_.SetParamTo((ctl - kParamCC) / kNumFxKnobs,
                               (ctl - kParamCC) % kNumFxKnobs + bank * kNumFxKnobs, value);
            else if (ctl >= kCompCC && ctl < kCompCC + kNumFxKnobs)
                fx_.SetComp(ctl - kCompCC + bank * kNumFxKnobs, value);
            else if (ctl >= kChaosNrpn && ctl < kChaosNrpn + kNumFxKnobs)
                fx_.SetParamTo(FX_CHAOS, ctl - kChaosNrpn + bank * kNumFxKnobs, value);
            else if (bank)
                return;
            else if (cc >= kLatchCC && cc < kLatchCC + kMidiFx)
                fx_.SetLatch(cc - kLatchCC, raw >= 64);
            else if (cc == kChaosLatchCC)
                fx_.SetLatch(FX_CHAOS, raw >= 64);
            else if (cc == kOutGainCC)
                SetOutGain(value);
            else if (cc == kInGainCC)
                SetInGain(value);
            else if (cc == kMixCC)
                SetMix(value);
            else if (cc == kHpCueCC)
                SetHpCue(value);
            else if (cc == kMonoCC && settings_.SetMono(raw >= 64))
                MasterChanged(System::GetNow());
            else if (cc == kMorphBarsCC)
                morph_bars_ = raw < 1 ? 1 : (raw > kMaxMorphBars ? kMaxMorphBars : raw);
            else if (cc == kMorphCC && raw < kNumSlots)
                RemoteMorph(raw);
            else if (cc == kStopMorphCC && raw >= 64)
                FreezeMorph();
            else if (cc == kFaderCC)
            {
                ScopedIrqBlocker irq;
                scene_ctl_.FaderTo(value);
            }
        }

        /** A morph over MIDI: as SHIFT + the scene key, tapped once per bar, and SHIFT let go,
         *  unless the hand holds it, whose release then lets it glide */
        void RemoteMorph(size_t slot)
        {
            const auto result = ScenePressed(slot, true, morph_bars_);
            if (result != SceneControls<PassthroughEngine>::Slot::REFUSED && !Shift())
                ReleaseMorph();
        }

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
                n = Put14(d, n, KnobToMidi14(in_gain_));
                n = Put14(d, n, KnobToMidi14(hp_cue_));
                n = Put14(d, n, static_cast<uint16_t>(engine_->FxBpm() * 10.f + .5f));
                // the mode switch: 1 if it stands up, plus SysEx's setting (kCmdSwitch) x2
                d[n++] = static_cast<uint8_t>((hw_->GetToggleState() ? 0 : 1) | midi_->Switch() << 1);
                d[n++] = static_cast<uint8_t>(fx_.Page()); // the FX knobs' page, 0 or 1
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
                midi_->TakeLoad(max, mean);
                n = Put14(d, n, max);
                n = Put14(d, n, mean);
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
        inline bool ShiftPressed() { return fx_.ShiftPressed(); }
        inline void ShiftUsed() { fx_.ShiftUsed(); }
        bool FreezeMorph()
        {
            ScopedIrqBlocker irq;
            return scene_ctl_.FreezeMorph();
        }
        /** SHIFT let go: a morph started with it glides now (FxMorph::Release), or the hand's
         *  crossfader ends where it is (SceneControls::EndFade). One on MIDI's crossfader
         *  (CC 118) stays MIDI's */
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

            // the compressor's knobs to the card, once they've been left alone a while
            if (fx_.TakeCompChanged())
                MasterChanged(now);
            // a failed write: the compressor's key blinks red, and it's tried again a few times
            if (scenes_->TakeMasterFailed())
            {
                master_refused_.Start(now);
                if (++master_tries_ <= kMasterSaveTries)
                {
                    master_unsaved_ = true;
                    master_changed_at_ = now;
                }
            }
            if (master_unsaved_ && now - master_changed_at_ > kMasterSaveDelayMs)
            {
                for (size_t p = 0; p < kNumFxParams; p++)
                    scenes_->master.comp[p] = fx_.CompParam(p);
                settings_.Store(scenes_->master);
                scenes_->RequestMasterSave();
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
                SetInGain(in_gain_ + inc);
            else
                SetHpCue(hp_cue_ - detents * kHpCueStep); // left towards the dry input
        }

        /** A tempo tap: with a loop, it refits the loop's beats; without one, it sets the
         *  tempo, unless MIDI clock runs (TempoClock.h) */
        void Tap()
        {
            const uint32_t now = System::GetNow();
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
        void SetInGain(float gain)
        {
            in_gain_ = fclamp(gain, 0.f, 1.f);
            engine_->SetInputGain(in_gain_);
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
                    // morphing to it: blinking on the FX clock's beats
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
            // knob the page doesn't use is dark, and page 2's pulse
            const size_t selected = fx_.Selected();
            const bool comp = selected == kCompSelected;
            const float* const* colors = comp ? kCompKnobColors : kFxSlots[selected].knob_colors;
            float pulse = 1.f;
            if (fx_.Page())
            {
                const float phase = static_cast<float>(now % kPage2PulseMs) / kPage2PulseMs;
                pulse = kPage2Low + (1.f - kPage2Low) * .5f * (1.f + cosf(phase * TWOPI_F));
            }
            const FxParams& knobs = fx_.Knobs();
            for (size_t k = 0; k < kNumFxKnobs; k++)
            {
                float rgb[3] = {0.f, 0.f, 0.f};
                if (fx_.KnobUsed(k))
                    KnobColor(knobs, fx_.ParamOf(k), fx_.Knob(k), colors, rgb);
                SetPthLedFloat(kFxKnobLeds[k], rgb[0] * pulse, rgb[1] * pulse, rgb[2] * pulse);
            }

            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                const float* color = kFxSlots[fx].key_color;
                // the meter (about the output's amplitude) on a dB scale: 0 at the floor, 1 at 0 dBFS
                const float db = 20.f * log10f(fmaxf(engine_->GetFxLevel(fx), 1e-6f));
                const float meter = fclamp(1.f - db / kFxMeterFloorDb, 0.f, 1.f);
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

        void DrawLooperLeds(uint32_t now)
        {
            const Looper& looper = engine_->looper;
            const Looper::State state = looper.GetState();

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
         *  white at its default, blue below and orange above, the same on every effect; any
         *  other goes from the effect's first colour to its last */
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
        uint8_t morph_bars_ = 1;       // a morph over MIDI's bars (midimap::kMorphBarsCC)
        FxScene put_scene_;            // a scene coming over SysEx (kCmdScenePut),
        int put_slot_ = kNoScene;      // for this slot,
        uint32_t put_parts_ = 0;       // with these parts so far
        uint32_t log_written_ = 0, log_failed_ = 0; // the log's files shown so far
        bool log_ok_ = false;
        LedSignal log_signal_; // the last file written, or not

        Looper::State last_looper_state_ = Looper::State::EMPTY;
        bool last_erasing_ = false;

        float out_gain_;
        float in_gain_;
        float mix_;
        float hp_cue_; // headphones: 0 = the master's mirror, 1 = the dry input
        uint8_t page_;

        PlayKeys<NormalPage> keys_;
        LedSignal loop_refused_; // a refused quantized record or tap
        TapTempo tap_tempo_;
        LedSignal tap_flash_;
        LedSignal select_flash_; // on the selected FX's key
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
        bool hand_fader_ = false;   // the transport knob has had the crossfader since SHIFT
                                    // went down (TransportTurned)
        LedSignal page_flash_;      // a page picked: its number in blinks
    };

} // namespace chompi

#pragma GCC pop_options
