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
 *  and its settings go to the master file as the finalizer's knobs do. VOLUME works there
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
 *  finalizer's press, or let go that soon before it, is SHIFT first, a select
 *  (FxControls::LateSelect); the latch gesture leaves far more time between them. An FX key
 *  back down that soon after its release is a bounce, and takes the release back
 *  (FxControls::Rebound).
 *
 *  An effect with a second mode (kFxModes, the phaser's frequency shifter, #306): SHIFT + its
 *  key selects it as any; the key again in the same hold, with no other key between, switches
 *  the mode (tap_fx_), however long after, and each further tap switches again. Only a press
 *  with SHIFT down counts (ShiftAt, its grace too), so the latch gesture (the key, then SHIFT)
 *  never does; letting go of SHIFT or another key starts the count again. Not on the Loops
 *  page, where the white keys are loops. The key lights in the mode's colour (FxSlots.h), at
 *  full for kModeFlashMs after a switch.
 *
 *  The finalizer's key (kCompKey, #345) selects it for the knobs, and flashes white. Its knobs
 *  go to the card (SceneStore's master file) kMasterSaveDelayMs after the last turn, so a
 *  sweep is one write. The key lights in the colour of the module the knobs show (kFinColors),
 *  never darker than kFinFloor, its gain reduction a pulse over that, and red over everything
 *  while the safety limiter works. Its knob LEDs are the module's meters while they rest
 *  (FinMeterLed), a knob's value for kFinValueMs after it's turned.
 *
 *
 *  Scenes: a recall sends only the parameters that change, within one audio block and at the
 *  fast slew (FxChain::FastSlew), so an FX the two scenes share runs on untouched. SHIFT +
 *  a scene key starts a timed sequence instead (#175, SceneControls.h): a morph (FxMorph.h),
 *  landing on a bar line, which the engine times; with SHIFT still held, the transport pressed
 *  makes it a drop, turned its crossfader (#66: the glide in the hand, until SHIFT is let go,
 *  ReleaseMorph; the transport LEDs show it), a second scene key a build-up to that one.
 *  The scene morphed to blinks white on the beat, the one it drops to red. PLAY has no part
 *  in it. A morph on MIDI's crossfader (CC 80) stays MIDI's: letting go of SHIFT doesn't end it.
 *  The card is written from MainLoop (SceneStore::Process), never here.
 *
 *  The Loops page and the Cubbi page (#70, LoopControls.h), sub-pages with the same keys:
 *  FRIZZ's own loops and TAPE's cubbis. SHIFT + the lower C# opens the Loops page, SHIFT + the
 *  lower D# the Cubbi page, from anywhere but the settings page (also from each other);
 *  SHIFT + the page's own key or a double tap of SHIFT (two clean taps within kDoubleTapMs)
 *  goes home. The keys the hand presses there come as kLoopsKeyBase plus the key (ui.h),
 *  MIDI's notes as ever, so MIDI plays the effects there and picks no loops. Their white keys
 *  are the bank's loops, the lower C# and D# the banks (each lit in the colour of the bank it
 *  leads to, dark at the end); the scene keys, the transport, PLAY,
 *  LOOP and VOLUME are the play page's; the scene modes and knobs 1-4 do nothing, dark.
 *  Opening one ends a scene mode, so a tap of SHIFT confirms nothing there; opening one
 *  without a loop blinks both bank keys red. SHIFT's LED shows the sub-page, dim in its
 *  colour (the bank's here), and nothing else may use it: white held, the confirm blink, the
 *  boot ripple. A later sub-page brings its own.
 *
 *  MIDI (MidiControl.h): notes and FRIZZ's SysEx keys and detents come as the hand's do (ui.h);
 *  the rest, Remote() takes from MainLoop: a controller sets a parameter, a latch, a gain, the
 *  mix or the loop's speed outright, as its knob or key would, a program change is a scene key,
 *  a morph over MIDI glides at once over the bars set; the finalizer's CCs aren't kept on the
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
#include "FinPresets.h"
#include "SettingsPage.h"
#include "TapTempo.h"
#include "SceneStore.h"
#include "EventLog.h"
#include "MidiControl.h"
#include "LedColors.h"
#include "passthroughEngine.h"
#include "temp_led_stuff.h"
#if !FRIZZ_BENCH
#include "LoopControls.h"
#endif

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
    static const uint32_t kModeFlashMs = 250;    // and at full in its new colour on a mode switch
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
    static const float kFinMeterDb = 12.f;    // the finalizer's key at full, dB reduced, and a
                                              // reduction's knob LED (FinKnobLed)
    static const float kFinFloor = .35f;      // the finalizer's key at least, so its colour reads
    static const float kFinMutedLevel = .05f; // a muted band's knob LED: a step of the chain, below kVuDim's glow
    static const uint32_t kFinValueMs = 1500; // a finalizer knob turned shows its value this long
    static const uint32_t kFinMuteFlashMs = 120; // and flashes this long as its band (un)mutes
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
    // red that it was refused or isn't on the card. Their LEDs: KeyLedOf (PanelKeys.h)
    static constexpr Hardware::SwId kSceneKeys[kNumSlots] = {
        Hardware::SwId::KEY_18, Hardware::SwId::KEY_19, Hardware::SwId::KEY_20,
        Hardware::SwId::KEY_21, Hardware::SwId::KEY_22,
    };
    struct SceneModeKey
    {
        SceneMode mode;
        Hardware::SwId key;
        const float* color;
    };
    static const SceneModeKey kSceneModeKeys[] = {
        {SceneMode::SAVE, Hardware::SwId::KEY_23, green},
        {SceneMode::COPY, Hardware::SwId::KEY_24, blue},
        {SceneMode::DELETE, Hardware::SwId::KEY_25, red},
    };
    static const uint32_t kScenePulseMs = 1000;     // the active scene, edited

    // The Loops page (#70): the hand's keys there come as these IDs plus their SwId (ui.h)
    static const uint16_t kLoopsKeyBase = 128;
    // those that are the play page's there (LoopsKey): SHIFT, PLAY, LOOP, the transport's and
    // VOLUME's presses, the scene keys
    static constexpr uint64_t kLoopsOwnKeys =
        1ull << static_cast<int>(Hardware::SwId::KEY_26) | 1ull << static_cast<int>(Hardware::SwId::KEY_27)
        | 1ull << static_cast<int>(Hardware::SwId::KEY_28) | 1ull << ENC_5_SW
        | 1ull << static_cast<int>(Hardware::SwId::ENC_6_SW) | 1ull << static_cast<int>(kSceneKeys[0])
        | 1ull << static_cast<int>(kSceneKeys[1]) | 1ull << static_cast<int>(kSceneKeys[2])
        | 1ull << static_cast<int>(kSceneKeys[3]) | 1ull << static_cast<int>(kSceneKeys[4]);
    static const uint32_t kDoubleTapMs = 400;  // SHIFT's taps home: each, and the gap
    static const uint32_t kLoadingBlinkMs = 100; // a planned loop not loaded yet
    // the banks A-H, around the colour wheel; not red (refused) nor white (flashes)
    static constexpr const float* kBankColors[8] = {orange, yellow, green, teal,
                                                    med_blue, blue, purple, pink};
    // the Cubbi page's banks a-e in TAPE's own colours (its NormalPage.h), so a TAPE player
    // knows them: purple, orange, and three LedColors.h hasn't (kept here, its shape is the
    // site's)
    static const float kTapeTeal[3] = {.14f, 1.f, .92f};
    static const float kTapeDarkOrange[3] = {.77f, .38f, .06f};
    static const float kTapeYellowGreen[3] = {.706f, 1.f, 0.f};
    static constexpr const float* kCubbiColors[5] = {purple, orange, kTapeTeal, kTapeDarkOrange,
                                                     kTapeYellowGreen};
    static const uint16_t kBothBankKeys = 0xFFFF; // bank_refused_key_: a page without a loop

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
            // the finalizer's knobs as they were left, from the card
            for (size_t p = 0; p < kNumFinParams; p++)
                fx_.SetFin(p, scenes_->master.fin[p]);
            fx_.TakeFinChanged();
            // and the settings page's: the mono input, the clock's factor, the LEDs, the input
            // gains and AUX's variant
            settings_.Init(engine_, midi_, hw_, scenes_->master);
            scene_ctl_.Init(scenes_->scenes, &fx_);
            fin_presets_.Init(&scenes_->master);
            keys_.Init(this);

            ResetSmtLeds();
            PthLedsOff();
        }

#if !FRIZZ_BENCH
        /** The loops from the card: rec the looper's own buffer */
        void InitLoops(LoopLoader *loader, int16_t *rec) { loops_.Init(engine_, loader, rec); }
        /** Each MainLoop pass, after the loader (LoopControls::Process) */
        void LoopsProcess(bool card) { loops_.Process(card); }
        /** The loop transfer moved a file (LoopTransfer::TakeMoved), changed a FRIZZ slot
         *  (TakeChanged) */
        void LoopMoved(int from, int to, bool swap) { loops_.Moved(from, to, swap); }
        void LoopChanged(int id) { loops_.Forget(id); }
        /** SLOT's flags 1 and 2 (LoopTransfer.h): it plays, it's loaded and ready */
        uint8_t LoopFlags(int id) const { return loops_.Flags(id); }
#endif

        /** The settings page's USB audio (SettingsPage::UsbAudio) */
        bool UsbAudio() const { return settings_.UsbAudio(); }

        /** A master change the hand or SysEx made that's still waiting out its rest, not yet
         *  asked to be saved (SceneStore::Reread) */
        bool MasterWaiting() const { return master_unsaved_; }

        /** The card taken back from the computer: what SceneStore::Reread took from it,
         *  applied as at boot. The master settings at once, the finalizer's knobs and the
         *  input gains gliding there as a turn does (and USB audio switched over MainLoop's
         *  way); what MIDI set without the card goes to the file's. The scenes for their next
         *  recall: what plays stays, an active one changed counts as edited, one emptied is no
         *  longer active (as over SysEx) */
        void Reread(uint8_t taken)
        {
            if (taken & SceneStore::kRereadMaster)
            {
                for (size_t p = 0; p < kNumFinParams; p++)
                    fx_.SetFin(p, scenes_->master.fin[p]);
                fx_.TakeFinChanged();
                settings_.Init(engine_, midi_, hw_, scenes_->master);
                midi_->SetChannel(scenes_->master.midi_channel);
                midi_->SetTransport(scenes_->master.midi_transport);
            }
            for (size_t s = kBlankSlot + 1; s < kNumSlots; s++)
                if ((taken >> s) & 1)
                {
                    if (!scenes_->scenes[s].used)
                        scene_ctl_.Emptied(s);
                    else if (static_cast<int>(s) == scene_ctl_.Active())
                        fx_.MarkEdited();
                }
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
        /** For ui.h: the Loops page is shown, its keys kLoopsKeyBase's */
        inline bool LoopsShown() const { return loops_shown_; }
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
                {
                    scene_ctl_.Cancel();
                    fin_presets_.Cancel(); // the finalizer's presets' mode too (#345)
                }
            }
            if (settings)
            {
                // SHIFT + VOLUME held there: a bug report, its blink over the transport LEDs;
                // SHIFT held from the play page counts, as for its keys
                settings_.PlayShift(Shift(), now);
                if (settings_.BugReportHeld(now))
                    log_->RequestWrite();
                // SHIFT + LOOP held there: the card lent or taken back (CardLend.h)
                if (settings_.CardHeld(now))
                    cardlend::Get().asked = true;
                // the limiting it sees meanwhile is old once the play page shows the key again
                engine_->TakeLimiterGain();
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
#if !FRIZZ_BENCH
            if (loops_shown_)
                DrawLoopsLeds(now);
            else
#endif
                DrawFxLeds(now);
            DrawSceneLeds(now);

            // CHOMPI key: blinking in the mode's colour while a tap would confirm a scene
            // action, otherwise white while it is acting as SHIFT; on a sub-page dim in its
            // colour (nothing else may use it)
            if (SceneArmed() && !keys_.ShiftCombo())
                PthLed(kChompiKeyLed, SceneModeColor(), BlinkOn(now, kPendingBlinkMs) ? 1.f : 0.f);
#if !FRIZZ_BENCH
            else if (loops_shown_ && !Shift())
                PthLed(kChompiKeyLed, BankColor(loops_.Bank()), kFxOffLevel);
#endif
            else
                PthLed(kChompiKeyLed, white, Shift() ? 1.f : 0.f);

            fill_led_data();
        }

        bool OnButton(uint16_t buttonID,
                      uint8_t numberOfPresses,
                      bool isRetriggering) override
        {
            const bool rising = numberOfPresses == 1;
            // a mode's taps (tap_fx_): another key between, SHIFT's release, start again
            if (rising ? tap_fx_ >= kNumFx || buttonID != static_cast<uint16_t>(kFxSlots[tap_fx_].key)
                       : buttonID == static_cast<uint16_t>(Hardware::SwId::KEY_26))
                tap_fx_ = kNumFx;
            // and the finalizer's module taps (tap_fin_) the same way
            if (rising ? buttonID != static_cast<uint16_t>(kCompKey)
                       : buttonID == static_cast<uint16_t>(Hardware::SwId::KEY_26))
                tap_fin_ = false;
#if !FRIZZ_BENCH
            // anything but SHIFT between its taps: no double tap
            if (buttonID % kLoopsKeyBase != static_cast<uint16_t>(Hardware::SwId::KEY_26))
                tap_one_ = tap_clean_ = false;
            if (buttonID >= kLoopsKeyBase)
                return LoopsKey(buttonID - kLoopsKeyBase, numberOfPresses);
#endif
            if (buttonID >= kSettingsKeyBase)
            {
                const int key = buttonID - kSettingsKeyBase;
                const uint32_t now = System::GetNow();
                settings_.Held(key, rising, now);
                if (key == static_cast<int>(Hardware::SwId::ENC_6_SW))
                    SettingsVolumePressed(rising, now);
                else if (rising && settings_.Key(key, Shift(), now))
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
                {
                    ScopedIrqBlocker irq;
                    scene_ctl_.ShiftDown();
                }
                keys_.Chompi(rising);
                if (!rising && held)
                    ReleaseMorph();
#if !FRIZZ_BENCH
                ShiftTap(rising, held);
#endif
                return true;
            }
            case static_cast<uint16_t>(Hardware::SwId::KEY_27):
                keys_.Play(rising);
                return true;
            case static_cast<uint16_t>(Hardware::SwId::KEY_28):
                keys_.Loop(rising, edge_ms_[1][static_cast<int>(Hardware::SwId::KEY_28)]);
                return true;
            default:
                break;
            }

            if (buttonID == static_cast<uint16_t>(kCompKey))
            {
                // the finalizer's key selects, and always flashes; held, the scene keys are
                // its presets and SHIFT its bypass (FinHeld)
                fin_down_ = rising;
                if (rising)
                {
                    // SHIFT + the key again in the same hold: the next module (#345), as an
                    // effect's second mode (#306)
                    const bool shift = ShiftAt(buttonID);
                    const bool next = shift && tap_fin_ && !LoopsShownNow();
                    FxKeyDown(!next);
                    fx_.FinKeyPressed(shift);
                    if (next)
                    {
                        fx_.NextModule();
                        mode_flash_.Start(System::GetNow(), kModeFlashMs);
                    }
                    tap_fin_ = shift;
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
                        // SHIFT + the key again in the same hold: its other mode (#306)
                        const bool mode = shift && tap_fx_ == fx && !LoopsShownNow();
                        FxKeyDown(shift && !mode);
                        fx_.KeyPressed(fx, true, shift);
                        if (mode)
                            SwitchMode(fx);
                        tap_fx_ = shift && (kFxModes >> fx & 1) ? fx : kNumFx;
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
                    const bool shift = Shift();
                    VolumePressed(true, shift);
                    if (shift)
                        SetMix(LoopExists() ? 1.f : 0.f);
                }
                else if (VolumePressed(false))
                    ShowVolumePage((page_ + 1) % kNumPages, System::GetNow());
                return true;
            }
            if (!rising)
                return true;

            // The keys and presses below act only on the way down. Only those that do
            // something count as a SHIFT combo (keys_.Used): a key without a function doesn't
            // cancel a confirm or a latch in the making.

#if !FRIZZ_BENCH
            // SHIFT + the lower C#: the Loops page; SHIFT + the lower D#: the Cubbi page
            if ((buttonID == static_cast<uint16_t>(Hardware::SwId::KEY_16)
                 || buttonID == static_cast<uint16_t>(Hardware::SwId::KEY_17))
                && Shift())
            {
                PageKey(buttonID, true);
                return true;
            }
#endif
            // transport press: back to 1x forward; SHIFT + press makes a held scene morph a drop
            // (#175), and otherwise does nothing (it wrote a bug report until v0.11, now held on
            // the settings page)
            if (buttonID == ENC_5_SW)
            {
                if (Shift())
                {
                    bool dropped;
                    {
                        ScopedIrqBlocker irq;
                        dropped = scene_ctl_.TransportPressed();
                    }
                    if (dropped)
                        keys_.Used();
                }
                else if (LoopExists())
                {
                    engine_->looper.ResetSpeed();
                    speed_chunk_ = 0.f;
                }
                return true;
            }
            for (size_t slot = 0; slot < kNumSlots; slot++)
            {
                if (buttonID == static_cast<uint16_t>(kSceneKeys[slot]))
                {
                    keys_.Used();
                    if (FinPresetsShown())
                        FinPresetPressed(slot);
                    else
                        ScenePressed(slot, Shift());
                    return true;
                }
            }
            for (const SceneModeKey& key : kSceneModeKeys)
            {
                if (buttonID == static_cast<uint16_t>(key.key) && FinPresetsShown())
                {
                    // the finalizer's presets' mode (#345), instead of the scenes'
                    keys_.Used();
                    scene_ctl_.Cancel();
                    fin_presets_.ModePressed(key.mode);
                    return true;
                }
                if (buttonID == static_cast<uint16_t>(key.key))
                {
#if !FRIZZ_BENCH
                    // MIDI's note on the Loops page does nothing, as the hand's (LoopsKey): a
                    // mode can't start where a tap of SHIFT confirms nothing
                    if (loops_shown_)
                        return true;
#endif
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
                    const bool muted = fx_.Muted(knob);
                    if (fx_.KnobPressed(knob, Shift()))
                        keys_.Used();
                    if (fx_.Selected() == kFinSelected)
                    {
                        // the finalizer: a band (un)muted flashes, a reset shows its value
                        if (fx_.Muted(knob) != muted)
                            mute_flash_[knob].Start(System::GetNow(), kFinMuteFlashMs);
                        else
                            fin_turned_[knob] = System::GetNow();
                    }
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
#if !FRIZZ_BENCH
            tap_one_ = tap_clean_ = false;
            if (encoderID < kNumFxKnobs && loops_shown_ && !show_settings_)
                return true; // knobs 1-4 do nothing on the Loops page
#endif
            if (encoderID == kTransportEncoder)
                TransportTurned(turns);
            else if (encoderID < kNumFxKnobs && show_settings_)
            {
                // knob 1 the LEDs' brightness there (SettingsPage.h, #176)
                if (encoderID == 0 && settings_.KnobTurned(turns, Shift()))
                    MasterChanged(System::GetNow());
            }
            else if (encoderID < kNumFxKnobs)
            {
                if (fx_.KnobUsed(encoderID))
                    keys_.Used();
                fx_.KnobTurned(encoderID, turns, Shift());
                knob_page_flash_.Stop(); // a turn shows what it turns at once
                fin_turned_[encoderID] = System::GetNow(); // on the finalizer: its value
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
            // the computer gone, or FRIZZ leaving the bus (USB audio switched): the releases of
            // what its notes and SysEx hold are lost, so MIDI lets go of them
            const bool host = midi_->UsbHost(), audio = settings_.UsbAudio();
            if ((usb_host_ && !host) || audio != usb_audio_)
                midi_->LetGo();
            usb_host_ = host;
            usb_audio_ = audio;
            uint8_t key;
            if (midi_->TakeFocus(key))
            {
                const size_t fx = FxOnWhiteKey(key);
                fx_.Select(fx < kNumFx ? fx : kFinSelected);
                Heard(feedback::kFocus);
            }
            MidiControl::Event event;
            while (midi_->TakeEvent(event))
                RemoteEvent(event, now);
            bool on;
            while (midi_->TakeLatch(key, on))
            {
                if (FxOnWhiteKey(key) < kNumFx)
                    fx_.SetLatch(FxOnWhiteKey(key), on);
                else
                    fx_.SetBypass(!on); // the finalizer's CC 22: on, or bypassed
                HeardKey(key, feedback::kLatchSlot);
            }
            uint8_t id, raw;
            // the finalizer's module first, so the focus's knobs after it reach the new one
            if (midi_->TakeValueOf(midimap::kFinModule, raw))
                RemoteValue(midimap::kFinModule, raw);
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
#if !FRIZZ_BENCH
            // a loop the hand switched to (#259): sent before the next sweep
            const int switched = loops_.TakeSwitched();
            if (switched != LoopControls::kNone)
                loop_fed_ = feedback ? switched : LoopControls::kNone;
#endif

            // one answer a pass (every 2 ms), so USB has sent the last before the next goes,
            // or else MIDI feedback's; the loop transfer's first (LoopTransfer.h), whose ack takes
            // no answer's place
            if (midi_->xfer_send && midi_->xfer_send())
                return;
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
            case Event::DROP:
                RemoteMorph(e.arg & 7, e.arg >> 3, e.kind == Event::DROP);
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
            case Event::INPUT:
                RemoteInput(e.arg & 3, e.arg >> 2);
                break;
            case Event::FIN_PRESET:
                RecallFinPreset(e.arg);
                break;
#if !FRIZZ_BENCH
            static_assert(midimap::kLoopBanks == loops::kBanks && midimap::kLoopPrograms == loops::kSlots
                              && midimap::kCubbiProgramBank == loops::kCubbiBank + 1,
                          "a loop's program change: bank 1 is the loader's 0, the cubbis' after FRIZZ's");
            case Event::LOOP:
            {
                // a loop from the card (#259), from any page: "with the sequence" means one
                // that waits to land, also held by the hand, not on the crossfader
                ScopedIrqBlocker irq;
                loops_.Remote(e.arg / loops::kSlots, e.arg % loops::kSlots, e.arg2,
                              engine_->FxMorphing() && !scene_ctl_.Fading());
                break;
            }
#endif
            }
        }

        /** CC 84 the source (value: PassthroughEngine's kAux, kMic or kUsb), 116 its effects
         *  (on: dry), 117 it live (#177): as the settings page's F#, G# and A#, but the card
         *  doesn't get them (#97) */
        MIDI_CONTROL_ONCE void RemoteInput(uint8_t which, uint8_t value)
        {
            if (which == 0)
                settings_.SetSource(value);
            else if (which == 1)
                settings_.SetDry(value != 0);
            else
                settings_.SetLive(value != 0);
            Heard(which == 0 ? feedback::kSource : which == 1 ? feedback::kDry : feedback::kLive);
        }

        /** A controller's value (midimap::Value): a knob, the finalizer's too, which the card
         *  doesn't get (#97 D6), a gain, the loop's speed or the crossfader */
        MIDI_CONTROL_ONCE void RemoteValue(uint8_t id, uint8_t raw)
        {
            using namespace midimap;
            static_assert(kKeyParams == kNumFxParams && kNumValues - kFinParam == kNumFinParams,
                          "a CC per parameter");
            const float value = MidiToKnob(raw, false);
            if (id < kOutGain)
            {
                const size_t fx = FxOnWhiteKey(id / kKeyParams), param = id % kKeyParams;
                if (fx < kNumFx)
                {
                    fx_.SetParamTo(fx, param, value);
                    HeardKey(id / kKeyParams, param);
                }
                else // the finalizer's, through the focus: the module shown's
                    RemoteFin(fx_.Module() * kNumFxKnobs + param, value);
            }
            else if (id >= kFinParam)
                RemoteFin(id - kFinParam, value);
            else if (id == kFinModule)
            {
                fx_.SetModule(raw / 32);
                Heard(feedback::kFinModule);
            }
            else if (id >= kFinMute)
            {
                fx_.SetMute(id - kFinMute, raw >= 64);
                Heard(static_cast<uint8_t>(feedback::kFinMutes + id - kFinMute));
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
            else if (id == kMode)
            {
                const size_t fx = FxOnWhiteKey(kModeKey);
                if (fx < kNumFx && fx_.Mode(fx) != (raw >= 64))
                    SwitchMode(fx);
                Heard(feedback::kMode);
            }
        }

        /** The finalizer's preset i (0-3), if one is stored there: its knobs glide there, every
         *  band unmuted (#345, #347). keep: the hand's, for the card; MIDI's CC 21 isn't (#97).
         *  The knobs' new values go out as feedback */
        MIDI_CONTROL_ONCE void RecallFinPreset(size_t i, bool keep = false)
        {
            if (i >= kFinPresets || !(scenes_->master.fin_presets_used >> i & 1))
                return;
            for (size_t p = 0; p < kNumFinParams; p++)
                fx_.SetFin(p, scenes_->master.fin_presets[i][p], keep);
            fx_.Unmute();
            fin_preset_ = static_cast<uint8_t>(i);
        }

        /** The finalizer's parameter p from MIDI: not for the card */
        void RemoteFin(size_t p, float value)
        {
            fx_.SetFin(p, value, false);
            HeardFin(p);
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
            if (item < kFinItems)
            {
                // the finalizer in focus: the module shown's knobs on CC 14-17
                const size_t slot = item - kFocusItems;
                if (fx_.Selected() == kFinSelected)
                    return slot < kNumFxKnobs && FinValue(fx_.Module() * kNumFxKnobs + slot, v);
                return KeyValue(WhiteKeyOfFx(fx_.Selected()), slot, v);
            }
            if (item < kFocus)
            {
                // the rest of the finalizer's channel, not where the global one is
                if (!channel || channel == kKeys)
                    return false;
                if (item < kFinModule)
                    return FinValue(kNumFxParams + item - kFinItems, v);
                if (item == kFinModule)
                {
                    v = static_cast<uint8_t>(fx_.Module() * 32);
                    return true;
                }
                v = fx_.Muted(item - kFinMutes) ? 127 : 0;
                return true;
            }
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
                // the middle of each third CC 84 reads (MidiControl.h): AUX 0, the mic 64, USB 127
                v = midimap::kSourceValues[engine_->Source()];
                return true;
            case kDry:
                v = engine_->Dry(engine_->Source()) ? 127 : 0;
                return true;
            case kLive:
                v = engine_->Live(engine_->Source()) ? 127 : 0;
                return true;
            case kMode:
                // on the phaser's channel, as its knobs: not where the global one is (CC 23 the
                // focus there), nor while a morph runs
                v = fx_.Mode(FxOnWhiteKey(kModeKey)) ? 127 : 0;
                return channel && kModeKey + 1 != channel && !fx_.Morphing() && !hand_fader_;
            }
            return false;
        }

        /** The finalizer's parameter p, if it has one there */
        bool FinValue(size_t p, uint8_t& v)
        {
            v = feedback::KnobTo7(fx_.FinParam(p < kNumFinParams ? p : 0));
            return FxControls<PassthroughEngine>::FinUsed(p);
        }

        /** White key key's knob 0-7 or latch (feedback::kLatchSlot); the finalizer's
         *  parameters 0-6 there, and its on or off on the latch's CC */
        bool KeyValue(size_t key, size_t slot, uint8_t& v)
        {
            const size_t fx = FxOnWhiteKey(key);
            if (fx == kNumFx)
            {
                if (slot == feedback::kLatchSlot)
                {
                    v = fx_.Bypassed() ? 0 : 127;
                    return true;
                }
                return slot < midimap::kFinPresetCC - midimap::kKnobCC && FinValue(slot, v);
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
        }
        /** The finalizer's parameter p, every CC it goes out on */
        void HeardFin(size_t p)
        {
            using namespace feedback;
            if (p < kNumFxParams)
                Heard(static_cast<uint8_t>((kKeys - 1) * kKeySlots + p));
            else
                Heard(static_cast<uint8_t>(kFinItems + p - kNumFxParams));
            if (fx_.Selected() == kFinSelected && p / kNumFxKnobs == fx_.Module())
                Heard(static_cast<uint8_t>(kFocusItems + p % kNumFxKnobs));
        }

        /** A pass of MIDI feedback's sweep: over USB if MIDI out goes there, into the jack's
         *  ring, without the effect channels if they don't count on the jack */
        void Feedback(uint32_t now)
        {
            const uint8_t global = midi_->Channel() ? midi_->Channel() : 16;
            const uint8_t ch = static_cast<uint8_t>(global - 1);
            const bool usb = midi_->GetMidiOut() == MidiOutPorts::TRS_USB;
            const bool block_trs = midi_->BlockTrs();
            // msg, its 3-byte CCs n of them (all on the global channel for another), over USB
            // and into the jack's ring: false to try again at the next pass
            auto send = [&](uint8_t* msg, size_t n) {
                uint8_t trs[MidiFeedback::kPerPass * 3];
                size_t m = 0;
                for (size_t i = 0; i < 3 * n; i += 3)
                    if (block_trs || (msg[i] & 0x0F) == ch)
                    {
                        trs[m++] = msg[i];
                        trs[m++] = msg[i + 1];
                        trs[m++] = msg[i + 2];
                    }
                if (midi_->feedback_trs.Room() < m)
                    return false;
                // USB busy: again at the next pass, a few times, then without it; without a
                // host, not at all (#84)
                if (usb && midi_->UsbHost() && !midi_->TrySendUsb(msg, 3 * n)
                    && ++feedback_usb_tries_ < kFeedbackUsbTries)
                    return false;
                feedback_usb_tries_ = 0;
                midi_->feedback_trs.Push(trs, m);
                return true;
            };
#if !FRIZZ_BENCH
            // a loop switched to by hand (#259): its bank (CC 0), its program change, and the
            // scenes' bank again, so a DAW's recording leaves FRIZZ's program changes on the
            // scenes: 3 CCs' worth, the PC's 2 bytes then CC 0's 3
            if (loop_fed_ != LoopControls::kNone)
            {
                const uint8_t bank = static_cast<uint8_t>(loop_fed_ / loops::kSlots + 1);
                uint8_t msg[8] = {static_cast<uint8_t>(0xB0 | ch), midimap::kBankCC, bank,
                                  static_cast<uint8_t>(0xC0 | ch), static_cast<uint8_t>(loop_fed_ % loops::kSlots),
                                  static_cast<uint8_t>(0xB0 | ch), midimap::kBankCC, 0};
                // the jack's ring takes them as they are: each starts with its status byte
                if (midi_->feedback_trs.Room() < 8
                    || (usb && midi_->UsbHost() && !midi_->TrySendUsb(msg, 8)
                        && ++feedback_usb_tries_ < kFeedbackUsbTries))
                    return;
                feedback_usb_tries_ = 0;
                midi_->feedback_trs.Push(msg, 8);
                loop_fed_ = LoopControls::kNone;
                return;
            }
#endif
            feedback_.Pass(now, global, [this](uint8_t item, uint8_t& v) { return FeedbackValue(item, v); }, send);
        }

        /** A morph (CC 3) or a drop (CC 83) over MIDI: as SHIFT + the scene key, tapped once per
         *  bar, for a drop the transport pressed, and SHIFT let go, unless the hand holds it,
         *  whose release then starts it (SceneControls::Remote) */
        void RemoteMorph(size_t slot, uint8_t bars, bool drop)
        {
            static_assert(midimap::kMorphBarsMax == kMaxMorphBars, "MIDI's bars, the morph's");
            bool started;
            {
                ScopedIrqBlocker irq;
                started = scene_ctl_.Remote(slot, bars, drop);
                if (started)
                {
#if !FRIZZ_BENCH
                    loops_.SequenceEnded();
#endif
                    hand_fader_ = false; // a new morph: no fader has it yet
                    if (!Shift())
                        engine_->ReleaseFxMorph();
                }
            }
            if (!started)
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
                // the input (#177): the mic 1 (else AUX), its effects dry 2, it live 4, USB 8
                // (#196, else AUX or the mic)
                d[n++] = static_cast<uint8_t>((engine_->MicInput() ? 1 : 0)
                                              | (engine_->Dry(engine_->Source()) ? 2 : 0)
                                              | (engine_->Live(engine_->Source()) ? 4 : 0)
                                              | (engine_->Source() == PassthroughEngine::kUsb ? 8 : 0));
                break;
            }
            case kCmdParams:
                // both pages of an effect's, or the finalizer's four modules
                if (q.a > kNumFx)
                    return;
                d[n++] = q.a;
                static_assert(1 + 2 * kNumFinParams <= MidiControl::kMaxReply, "the finalizer's fit");
                for (size_t p = 0; p < (q.a == kNumFx ? kNumFinParams : kNumFxParams); p++)
                    n = Put14(d, n, KnobToMidi14(q.a == kNumFx ? fx_.FinParam(p) : fx_.Param(q.a, p)));
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
                n = loopxfer::Put7(d, n, over, 3);
                n = loopxfer::Put7(d, n, blocks, 3);
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
                    info.crc = crash::Crc32(info.image, info.image_len);
                    info.crc_done = true;
                }
                d[n++] = kVersion;
                d[n++] = info.kind;
                n = loopxfer::Put7(d, n, info.crc, 5);
                n = loopxfer::Put7(d, n, info.image_len, 3);
                n = loopxfer::Put7(d, n, System::GetNow(), 5);
                n = loopxfer::Put7(d, n, info.passes, 5);
                n = loopxfer::Put7(d, n, info.reset_cause, 5);
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
                uint16_t values[20];
                static_assert(kNumFxParams <= 20 && kNumFx <= 20 && sizeof(masterfile::kLines) / sizeof(masterfile::kLines[0]) <= 20
                                  && 5 + 2 * 20 <= MidiControl::kMaxReply, "DevValues' groups fit");
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
                n = loopxfer::Put7(d, n, info.seed, 5);
                for (size_t i = 0; i < FxChain::kRandoms; i++)
                    n = loopxfer::Put7(d, n, engine_->RandomState(i), 5);
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
                    n = loopxfer::Put7(d, n, b, 2);
                for (size_t i = 0; i < 6; i++)
                    n = loopxfer::Put7(d, n, hw_->mp_buff_[i], 2);
                break;
            }
            case CRASH:
            {
                // after a power-off the backup SRAM holds noise: zeros for that
                const bool valid = crash::Valid(info.crash->record);
                const crash::Record r = valid ? info.crash->record : crash::Record{};
                if (group == 2)
                    crash::Clear(info.crash->record);
                if (group > 2)
                    status = BAD;
                else if (group == 2)
                    d[n++] = 0;
                else if (group == 0)
                {
                    // the option bytes whether or not a crash is recorded
                    d[n++] = valid ? kCrashValid : 0;
                    d[n++] = static_cast<uint8_t>(r.kind);
                    const uint32_t words[] = {r.count, r.ms, r.crc, r.len, r.rsr, r.sp, info.option_bytes};
                    const uint8_t groups[] = {3, 5, 5, 3, 5, 5, 5};
                    for (size_t i = 0; i < sizeof(groups); i++)
                        n = loopxfer::Put7(d, n, words[i], groups[i]);
                }
                else
                    for (uint32_t w : {r.pc, r.lr, r.xpsr, r.cfsr, r.hfsr, r.mmfar, r.bfar})
                        n = loopxfer::Put7(d, n, w, 5);
                break;
            }
#if FRIZZ_BENCH
            case TEST:
                // the audio callback's take theirs as they come (MidiControl::SysEx)
                if (group == kHangMain)
                    info.hang = true;
                else if (group == kFaultMain)
                    d[n++] = static_cast<uint8_t>(*reinterpret_cast<volatile uint32_t*>(0x2FFF0000u) & 0x7F);
                else
                    status = BAD;
                break;
#else
            case TEST:
                status = NOT_HERE;
                break;
#endif
            case LOOPS:
            {
                const int got = info.loops ? info.loops(q.arg, q.len, d + n) : 0;
                status = !info.loops ? NOT_HERE : (got < 0 ? BAD : OK);
                n += got > 0 ? static_cast<size_t>(got) : 0;
                break;
            }
            case USB:
            {
                const int got = info.usb ? info.usb(q.arg, q.len, d + n) : 0;
                status = !info.usb ? NOT_HERE : (got < 0 ? BAD : OK);
                n += got > 0 ? static_cast<size_t>(got) : 0;
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
            return loopxfer::Put7(d, n, v > 0.f ? static_cast<uint32_t>(v * by + .5f) : 0u, groups);
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
            n = loopxfer::Put7(d, n, clock.GetTicks(), 5);
            n = loopxfer::Put7(d, n, clock.GetLocks(), 3);
            n = PutScaled(d, n, tempo.GetFxBpm(), 100.f, 3);
            n = loopxfer::Put7(d, n, static_cast<uint32_t>(tempo.GetTempo()), 2);
            n = loopxfer::Put7(d, n, tempo.Position(), 3);
            return loopxfer::Put7(d, n, tempo.LoopPulses(), 3);
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
            n = loopxfer::Put7(d, n, length, 4);
            n = PutScaled(d, n, looper.GetPosition(), static_cast<float>(length), 4);
            n = loopxfer::Put7(d, n, looper.GetBeats(), 3);
            d[n++] = static_cast<uint8_t>(looper.GetSemitones() + 64);
            n = PutScaled(d, n, looper.GetSpeed() + 2.f, 1000.f, 2);
            n = PutScaled(d, n, looper.GetActualSpeed() + 2.f, 1000.f, 2);
            return loopxfer::Put7(d, n, looper.Rewinds(), 3);
        }

        /** LEVELS: part 0 the effects' meters; part 1 the VU meter, the finalizer's reduction
         *  and the peaks since the last ask, the meters started by the first; part 2 the
         *  finalizer's meters: the input's peak and the bands' since the last ask (full scale
         *  8192, up to twice it), their reductions (dB down, x100), their correlations (+1 at
         *  16383, -1 at 0) and the limiter's deepest reduction since the last ask, the band
         *  meters run while key 15 shows EQ or Width */
        MIDI_CONTROL_ONCE size_t DevLevels(uint8_t part, uint8_t* d, size_t n)
        {
            using namespace dev;
            if (part == 0)
            {
                for (size_t fx = 0; fx < kNumFx; fx++)
                    n = PutScaled(d, n, engine_->GetFxLevel(fx), 16383.f, 2);
                return n;
            }
            if (part == 2)
            {
                Finalizer& fin = engine_->Fin();
                n = PutScaled(d, n, fin.TakeInputPeak(), 8192.f, 2);
                for (size_t b = 0; b < Finalizer::kBands; b++)
                    n = PutScaled(d, n, fin.TakeBandPeak(b), 8192.f, 2);
                for (size_t b = 0; b < Finalizer::kBands; b++)
                    n = PutScaled(d, n, -fin.BandReduction(b), 100.f, 2);
                for (size_t b = 0; b < Finalizer::kBands; b++)
                    n = PutScaled(d, n, .5f + .5f * fin.BandCorrelation(b), 16383.f, 2);
                return PutScaled(d, n, -fin.TakeLimiterReduction(), 100.f, 2);
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
            n = PutScaled(d, n, -engine_->GetFinReduction(), 100.f, 2);
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
            if (group < kNumFx)
            {
                for (size_t p = 0; p < kNumFxParams; p++)
                    v[p] = KnobToMidi14(fx_.Param(group, p));
                return kNumFxParams;
            }
            if (group == kGroupFin)
            {
                for (size_t p = 0; p < kNumFinParams; p++)
                    v[p] = KnobToMidi14(fx_.FinParam(p));
                return kNumFinParams;
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
            else if (group == kGroupFin && FxControls<PassthroughEngine>::FinUsed(index))
            {
                fx_.SetFin(index, value);
                HeardFin(index);
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
                    TurnInGain(value, System::GetNow()); // as VOLUME's page 2: to the card
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

        /** A scene's part for kCmdSceneGet: kFxPerPart effects' knobs on one page, 14 bits
         *  each (parts 0-3 page 1, 4-7 page 2), or (the last) whether it's used, its latches
         *  and its modes (FxScene::modes, #306) */
        static size_t ScenePart(const FxScene& scene, uint8_t part, uint8_t* d, size_t n)
        {
            if (part == kSceneParts - 1)
            {
                d[n++] = scene.used ? 1 : 0;
                return Put14(d, Put14(d, n, scene.latched), scene.modes);
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
                // its modes, from a tool that sends them (#306): else none
                put_scene_.modes = q.len >= 5 ? static_cast<uint16_t>(((v[3] << 7) | v[4]) & kFxModes) : 0;
                scenes_->scenes[q.a] = put_scene_;
                put_slot_ = kNoScene;
                // the sound stays, so it no longer matches the active scene (as COPY onto it);
                // an empty one is no scene to be active (as DELETE)
                if (!put_scene_.used)
                    scene_ctl_.Emptied(q.a);
                else if (q.a == scene_ctl_.Active())
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
        inline bool SceneArmed() const { return scene_ctl_.Armed() || fin_presets_.Armed(); }
        /** SHIFT going down: SHIFT + key after all if an FX key's or the compressor's press
         *  came at most kShiftGraceMs before it, a select, which a plain press of an FX key
         *  takes back (FxControls::LateSelect); otherwise the FX keys held will latch */
        bool ShiftPressed()
        {
#if !FRIZZ_BENCH
            if (loops_shown_)
                return fx_.ShiftPressed(); // its white keys are loops: no select
#endif
            const uint16_t shift = static_cast<uint16_t>(Hardware::SwId::KEY_26);
            bool select = Grace(static_cast<uint16_t>(kCompKey), true, shift);
            if (select)
                tap_fin_ = true; // the first of the finalizer's module taps
            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                if (Grace(static_cast<uint16_t>(kFxSlots[fx].key), true, shift))
                {
                    // a select whatever LateSelect says: SHIFT and the key in one block come
                    // SHIFT first (ui.h's Tell, by SwId), so LateSelect finds no press yet; the
                    // key's, still to come, selects with SHIFT held, and this SHIFT is a combo,
                    // not a tap. Not a slip: keep it
                    fx_.LateSelect(fx);
                    select = true;
                    if (kFxModes >> fx & 1)
                        tap_fx_ = fx; // the first of a mode's taps (#306)
                }
            }
            // key 15 held, then SHIFT: the finalizer bypassed or on again (#345), unless a mode
            // waits for SHIFT's confirm
            if (!select && fin_down_ && fin_presets_.Mode() == SceneMode::NONE && !scene_ctl_.Armed())
            {
                fx_.SetBypass(!fx_.Bypassed());
                Heard(static_cast<uint8_t>((feedback::kKeys - 1) * feedback::kKeySlots + feedback::kLatchSlot));
                fx_.ShiftUsed();
                return true;
            }
            if (!select)
                return fx_.ShiftPressed();
            fx_.ShiftUsed(); // as SHIFT, then the key, would leave the keys held long before
            select_flash_.Start(System::GetNow(), kSelectFlashMs);
            return true;
        }
        inline void ShiftUsed() { fx_.ShiftUsed(); }
        /** CC 81: the sequence stopped where it is, and the whole plan with it: a loop planned
         *  alone too (#259) */
        bool FreezeMorph()
        {
            ScopedIrqBlocker irq;
#if !FRIZZ_BENCH
            loops_.Cancel();
#endif
            return scene_ctl_.FreezeMorph();
        }
        /** SHIFT let go: a sequence started with it runs now, its bars counted from here
         *  (FxMorph::Release), or in a hold that began with a white key to the loop's pass end
         *  (ReleaseFxMorphAt, LoopControls::WithScene); or the hand's crossfader ends where it
         *  is (SceneControls::EndFade), its loops to the nearer side. One on MIDI's crossfader
         *  (CC 80) stays MIDI's */
        void ReleaseMorph()
        {
            ScopedIrqBlocker irq;
#if !FRIZZ_BENCH
            const bool passes = loops_.WithScene() && scene_ctl_.HoldSequence();
            loops_.ShiftUp(scene_ctl_.FaderPos());
            if (!(scene_ctl_.ShiftUp() && scene_ctl_.EndFade()))
            {
                if (passes)
                    engine_->ReleaseFxMorphAt(loops_.At());
                else
                    engine_->ReleaseFxMorph();
            }
#else
            if (!(scene_ctl_.ShiftUp() && scene_ctl_.EndFade()))
                engine_->ReleaseFxMorph();
#endif
            hand_fader_ = false;
        }
        inline void Refused() { loop_refused_.Start(System::GetNow()); }
        inline Looper::State LooperState() const { return engine_->looper.GetState(); }
        inline bool CanRecordQuantized() { return engine_->looper.CanRecordQuantized(); }
        inline void StartRecording(bool quantized)
        {
#if !FRIZZ_BENCH
            // a loop from the card still landing in the recording's buffer (LoopControls.h)
            if (loops_.WritingRec())
            {
                Refused();
                return;
            }
#endif
            engine_->looper.StartRecording(quantized);
        }
        inline void StopRecording() { engine_->looper.StopRecording(); }
        inline void TogglePlay() { engine_->looper.TogglePlay(); }
        inline void Erase() { engine_->looper.Erase(); }
        inline void EraseAtEnd() { engine_->looper.EraseAtEnd(); }
        inline void CancelErase() { engine_->looper.CancelErase(); }
        inline bool ErasePending() const { return engine_->looper.IsErasePending(); }

        /** fx's other mode (#306), SHIFT + its key twice or MIDI's CC: its key flashes in the
         *  new mode's colour */
        void SwitchMode(size_t fx)
        {
            fx_.SetMode(fx, !fx_.Mode(fx));
            mode_flash_.Start(System::GetNow(), kModeFlashMs);
        }
        inline bool LoopsShownNow() const
        {
#if !FRIZZ_BENCH
            return loops_shown_;
#else
            return false;
#endif
        }

        /** An FX or the compressor's key going down: tells PlayKeys, and flashes the
         *  key white for a select */
        void FxKeyDown(bool flash)
        {
            keys_.FxKey();
            if (flash)
                select_flash_.Start(System::GetNow(), kSelectFlashMs);
        }

#if !FRIZZ_BENCH
        // ---- the Loops page (#70, LoopControls.h) ----

        /** A key the hand pressed or let go there (kLoopsKeyBase's): the play page's own as
         *  on it, the banks, the loops; the scene modes and the knobs' presses nothing */
        bool LoopsKey(uint16_t key, uint8_t presses)
        {
            using S = Hardware::SwId;
            if (kLoopsOwnKeys >> key & 1)
                return OnButton(key, presses, false);
            if (presses != 1)
                return true;
            if (key == static_cast<uint16_t>(S::KEY_16) || key == static_cast<uint16_t>(S::KEY_17))
            {
                // SHIFT + the page's key goes home, SHIFT + the other's to the other page
                if (Shift())
                {
                    const bool cubbi = key == static_cast<uint16_t>(S::KEY_17);
                    PageKey(key, cubbi != loops_.Cubbi());
                }
                else if (!loops_.StepBank(key == static_cast<uint16_t>(S::KEY_16) ? -1 : 1))
                {
                    bank_refused_key_ = key;
                    bank_refused_.Start(System::GetNow()); // no bank further that way
                }
                return true;
            }
            for (size_t w = 0; w < kNumWhiteKeys; w++)
                if (key == static_cast<uint16_t>(kWhiteKeys[w]))
                {
                    keys_.Used();
                    LoopKeyPressed(w);
                }
            return true;
        }

        /** SHIFT + the lower C# (the Loops page) or D# (the Cubbi page), with on whether its
         *  page shows after: from the play page, MIDI's notes on the Loops page too, always */
        void PageKey(uint16_t key, bool on)
        {
            keys_.Used();
            ShowLoops(on, key == static_cast<uint16_t>(Hardware::SwId::KEY_17));
        }

        /** A white key there: its loop, at once or planned (LoopControls::Press). SHIFT + the
         *  planned one in a later hold cancels the plan, its scene sequence stopping where it is */
        void LoopKeyPressed(size_t slot)
        {
            LoopControls::Key key;
            {
                ScopedIrqBlocker irq;
                key = loops_.Press(slot, Shift(), scene_ctl_.HoldSequence(), scene_ctl_.HandFader(),
                                   scene_ctl_.HoldSpent());
                if (key == LoopControls::Key::CANCELLED)
                {
                    scene_ctl_.FreezeMorph();
                    scene_ctl_.Spend();
                }
            }
            if (key == LoopControls::Key::REFUSED)
                LoopRefused(slot);
        }

        void LoopRefused(size_t slot)
        {
            loop_refused_slot_ = slot;
            loop_refused_key_.Start(System::GetNow());
        }

        /** The Loops page or the Cubbi page opened (a scene mode ends; without a loop on it
         *  the bank keys blink red), or left */
        void ShowLoops(bool on, bool cubbi = false)
        {
            ResetSmtLeds(); // the keys a page doesn't draw would keep the other's colours
            loops_shown_ = on;
            loop_refused_key_.Stop();
            bank_refused_.Stop();
            if (on)
            {
                scene_ctl_.Cancel();
                fin_presets_.Cancel();
                if (!loops_.Show(cubbi))
                {
                    bank_refused_key_ = kBothBankKeys;
                    bank_refused_.Start(System::GetNow());
                }
            }
        }

        static const float* BankColor(size_t bank)
        {
            return bank < loops::kCubbiBank ? kBankColors[bank] : kCubbiColors[bank - loops::kCubbiBank];
        }

        /** SHIFT went down or up (held: its press was seen): two clean taps on a sub-page, each
         *  and the gap between them within kDoubleTapMs, go home. Anything else meanwhile
         *  (OnButton, OnEncoderTurned) isn't a tap */
        void ShiftTap(bool down, bool held)
        {
            const int k = static_cast<int>(Hardware::SwId::KEY_26);
            if (down)
            {
                tap_two_ = tap_one_ && edge_ms_[1][k] - tap_up_ <= kDoubleTapMs;
                tap_one_ = false;
                tap_clean_ = true;
                return;
            }
            const bool tap = held && tap_clean_ && edge_ms_[0][k] - edge_ms_[1][k] <= kDoubleTapMs;
            tap_clean_ = false;
            tap_one_ = tap && !tap_two_;
            tap_up_ = edge_ms_[0][k];
            if (tap && tap_two_ && loops_shown_)
                ShowLoops(false);
        }

        /** The Loops page's keys: the slots in the bank's colour, C# and D# in the colours of
         *  the banks they lead to (dark at the end); knobs 1-4 dark (PthLedsOff) */
        void DrawLoopsLeds(uint32_t now)
        {
            // the limiting it sees meanwhile is old once the play page shows the key again
            engine_->TakeLimiterGain();
            const float* color = BankColor(loops_.Bank());
            const uint32_t pos = engine_->FxClockPosition();
            const bool beat = pos % kPulsesPerBeat < kPulsesPerBeat / 2;
            for (size_t w = 0; w < kNumWhiteKeys; w++)
            {
                const float* c = color;
                float level = 0.f;
                if (w == loop_refused_slot_ && loop_refused_key_.Active(now))
                {
                    c = red;
                    level = loop_refused_key_.BlinkLevel(now);
                }
                else
                    switch (loops_.SlotLed(w))
                    {
                    case LoopControls::Led::DARK:
                        break;
                    case LoopControls::Led::BAD:
                        c = red;
                        level = kFxOffLevel;
                        break;
                    case LoopControls::Led::FILLED:
                        level = kFxOffLevel;
                        break;
                    case LoopControls::Led::PLAYING:
                        level = 1.f;
                        break;
                    case LoopControls::Led::PLANNED:
                        level = beat ? 1.f : 0.f; // the waiting scene key's rhythm
                        break;
                    case LoopControls::Led::LOADING:
                        level = BlinkOn(now, kLoadingBlinkMs) ? 1.f : 0.f;
                        break;
                    }
                SmtLed(KeyLedOf(kWhiteKeys[w]), c, level);
            }
            // C# and D# in the colour of the bank they lead to, dark at the end
            for (Hardware::SwId key : {Hardware::SwId::KEY_16, Hardware::SwId::KEY_17})
            {
                const int to = loops_.Neighbour(key == Hardware::SwId::KEY_16 ? -1 : 1);
                if ((static_cast<uint16_t>(key) == bank_refused_key_ || bank_refused_key_ == kBothBankKeys)
                    && bank_refused_.Active(now))
                    SmtLed(KeyLedOf(key), red, bank_refused_.BlinkLevel(now));
                else
                    SmtLed(KeyLedOf(key), to == LoopControls::kNone ? color : BankColor(static_cast<size_t>(to)),
                           to == LoopControls::kNone ? 0.f : 1.f);
            }
        }
#endif

        /** Once per frame, before drawing: what follows from time and the looper's state */
        void Update(uint32_t now)
        {
            // the detents counted for the crossfader, once they're old enough (#175)
            if (scene_ctl_.FaderWaiting())
            {
                ScopedIrqBlocker irq;
                if (scene_ctl_.Tick(now))
                {
                    hand_fader_ = true;
#if !FRIZZ_BENCH
                    loops_.FaderTook();
#endif
                }
            }
#if !FRIZZ_BENCH
            // a loop from the card started on an empty looper: wet, as a recording closing
            if (loops_.TakeStarted())
                SetMix(1.f);
            const int failed = loops_.TakeFailed();
            if (failed != LoopControls::kNone && static_cast<size_t>(failed) / loops::kSlots == loops_.Bank())
                LoopRefused(static_cast<size_t>(failed) % loops::kSlots);
#endif

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

            // the finalizer's knobs to the card, once they've been left alone a while: those
            // the hand or SysEx set, not MIDI's CCs (#97 D6)
            if (const uint16_t fin = fx_.TakeFinChanged())
            {
                for (size_t p = 0; p < kNumFinParams; p++)
                    if ((fin >> p) & 1)
                        scenes_->master.fin[p] = fx_.FinParam(p);
                MasterChanged(now);
            }
            // a failed write: the finalizer's key blinks red, and the settings page's key
            // changed last, and it's tried again a few times
            if (scenes_->TakeMasterFailed())
            {
                FinFlash(now, false);
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
                FinFlash(now, true);
            }
            if (master_unsaved_ && now - master_changed_at_ > kMasterSaveDelayMs)
            {
                settings_.Store(scenes_->master);
                scenes_->RequestMasterSave();
                master_saving_ = true;
                master_unsaved_ = false;
            }
        }

        /** VOLUME turned on the play page: its page's, SHIFT + turn the mix */
        void VolumeTurned(float detents)
        {
            if (Shift())
                SetMix(mix_ + detents * kMixStep);
            else
                VolumeTurnedOn(detents, page_, System::GetNow());
        }
        /** VOLUME turned on a page, the play page's or the settings page's */
        void VolumeTurnedOn(float detents, uint8_t page, uint32_t now)
        {
            const float inc = detents * kVolumeStep;

            if (page == kOutGainPage)
                SetOutGain(out_gain_ + inc);
            else if (page == kInGainPage)
                TurnInGain(settings_.InGain() + inc, now);
            else
                SetHpCue(hp_cue_ - detents * kHpCueStep); // left towards the dry input
        }
        /** VOLUME's press going down (shift: as SHIFT + press) or up. True on the release
         *  that picks the next page: not after SHIFT + press, nor after a press the boot page
         *  had */
        bool VolumePressed(bool down, bool shift = false)
        {
            if (down)
            {
                vol_down_ = true;
                vol_shift_ = shift;
                return false;
            }
            const bool next = vol_down_ && !vol_shift_;
            vol_down_ = false;
            return next;
        }
        /** VOLUME on page: its number blinks */
        void ShowVolumePage(uint8_t page, uint32_t now)
        {
            page_ = page;
            page_flash_.Start(now, (page_ + 1) * 2 * kSignalBlinkMs);
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
        /** The input gain by hand or SysEx: for the card, written as the settings are */
        void TurnInGain(float gain, uint32_t now)
        {
            SetInGain(gain);
            MasterChanged(now);
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
            if (!Shift() && !settings_.ShiftHeld())
                VolumeTurnedOn(detents, SettingsVolumePage(), vol_touched_);
        }
        /** VOLUME pressed on the settings page: on its release, page 1 or 2, unless it was SHIFT
         *  + press (the bug report's hold, SettingsPage::Held) */
        void SettingsVolumePressed(bool down, uint32_t now)
        {
            vol_touched_ = now;
            if (VolumePressed(down, down && (Shift() || settings_.ShiftHeld())))
                ShowVolumePage(SettingsVolumePage() == kInGainPage ? kOutGainPage : kInGainPage, now);
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
                rgb[0] = rgb[1] = rgb[2] = page_flash_.BlinkLevel(now);
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

        /** A scene key, the hand's or a program change's. A refusal blinks the slot */
        SceneControls<PassthroughEngine>::Slot ScenePressed(size_t slot, bool shift)
        {
            SceneControls<PassthroughEngine>::Slot result;
            {
                // a recall within one audio block
                ScopedIrqBlocker irq;
#if !FRIZZ_BENCH
                result = scene_ctl_.Press(slot, shift, loops_.HoldPasses());
#else
                result = scene_ctl_.Press(slot, shift);
#endif
                if (result == SceneControls<PassthroughEngine>::Slot::MORPH)
                    hand_fader_ = false; // a new morph: no fader has it yet
#if !FRIZZ_BENCH
                loops_.Scene(result); // the loop planned with it, or ended with it
#endif
            }
            if (result == SceneControls<PassthroughEngine>::Slot::REFUSED)
                SceneRefusedBlink(slot);
            return result;
        }

        void ConfirmScene()
        {
            if (fin_presets_.Armed())
            {
                // a preset of the finalizer's: to the card at once, flashing once written
                const SceneMode mode = fin_presets_.Mode();
                fin_flash_ = fin_presets_.Confirm(fx_.FinParams());
                if (mode == SceneMode::SAVE)
                    fin_preset_ = static_cast<uint8_t>(fin_flash_);
                else if (mode == SceneMode::DELETE && fin_flash_ == fin_preset_)
                    fin_preset_ = kNoFinPreset;
                fin_flash_waiting_ = true;
                fin_flash_signal_.Stop();
                MasterChanged(System::GetNow());
                master_changed_at_ = System::GetNow() - kMasterSaveDelayMs - 1;
                return;
            }
            const int slot = scene_ctl_.Confirm();
            if (slot != kNoScene)
                FlashWhenSaved(slot);
        }

        /** The scene keys show the finalizer's presets: key 15 held, or a mode for them */
        inline bool FinPresetsShown() const
        {
            return (fin_down_ || fin_presets_.Mode() != SceneMode::NONE) && !LoopsShownFin();
        }
        inline bool LoopsShownFin() const
        {
#if !FRIZZ_BENCH
            return loops_shown_;
#else
            return false;
#endif
        }

        /** A scene key while the presets show: scenes 1-4's keys are presets 1-4, the blank
         *  scene's nothing. In a mode it picks; else it recalls a stored one, the knobs gliding
         *  there, every band unmuted; one it can't blinks red */
        void FinPresetPressed(size_t slot)
        {
            if (slot == kBlankSlot)
                return;
            const size_t p = slot - 1;
            const bool ok = fin_presets_.Mode() != SceneMode::NONE ? fin_presets_.Pressed(p)
                                                                    : fin_presets_.Used(p);
            if (!ok)
            {
                fin_refused_ = static_cast<int>(slot);
                fin_refused_signal_.Start(System::GetNow());
                return;
            }
            if (fin_presets_.Mode() == SceneMode::NONE)
                RecallFinPreset(p, true);
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
            // the finalizer's presets' mode where one runs (#345)
            const SceneMode mode = fin_presets_.Mode() != SceneMode::NONE ? fin_presets_.Mode() : scene_ctl_.Mode();
            for (const SceneModeKey& key : kSceneModeKeys)
            {
                if (mode == key.mode)
                    return key.color;
            }
            return white;
        }

        /** A preset's write came out: its key flashes white, or red where it failed */
        void FinFlash(uint32_t now, bool ok)
        {
            if (!fin_flash_waiting_)
                return;
            fin_flash_waiting_ = false;
            fin_flash_ok_ = ok;
            fin_flash_signal_.Start(now);
        }

        /** The finalizer's presets on scenes 1-4's keys (FinPresetsShown): in a mode as the
         *  scenes' are, its colour on the mode keys; else the stored ones dim, the recalled one
         *  white; a write's flash, a refusal's red */
        void DrawFinPresetLeds(uint32_t now)
        {
            const SceneMode mode = fin_presets_.Mode();
            const float* mode_color = white;
            for (const SceneModeKey& key : kSceneModeKeys)
            {
                SmtLed(KeyLedOf(key.key), key.color, mode == key.mode ? 1.f : kFxOffLevel);
                if (mode == key.mode)
                    mode_color = key.color;
            }
            const bool blink_on = BlinkOn(now, kPendingBlinkMs);
            for (size_t slot = 0; slot < kNumSlots; slot++)
            {
                const int s = static_cast<int>(slot), p = s - 1;
                const float* color = white;
                float level = 0.f;
                if (slot == kBlankSlot)
                    level = 0.f;
                else if (p == fin_flash_ && fin_flash_waiting_)
                    level = 1.f;
                else if (p == fin_flash_ && fin_flash_signal_.Active(now))
                {
                    color = fin_flash_ok_ ? white : red;
                    level = fin_flash_signal_.BlinkLevel(now);
                }
                else if (s == fin_refused_ && fin_refused_signal_.Active(now))
                {
                    color = red;
                    level = fin_refused_signal_.BlinkLevel(now);
                }
                else if (mode != SceneMode::NONE)
                {
                    color = mode_color;
                    if (p == fin_presets_.Source())
                        level = 1.f;
                    else if (p == fin_presets_.Selected())
                        level = blink_on ? 1.f : 0.f;
                    else if (fin_presets_.Valid(static_cast<size_t>(p)))
                        level = kFxOffLevel;
                }
                else if (p == fin_preset_ && fin_presets_.Used(static_cast<size_t>(p)))
                    level = 1.f;
                else if (fin_presets_.Used(static_cast<size_t>(p)))
                    level = kFxOffLevel;
                SmtLed(KeyLedOf(kSceneKeys[slot]), color, level);
            }
        }

        void DrawSceneLeds(uint32_t now)
        {
            if (FinPresetsShown())
            {
                DrawFinPresetLeds(now);
                return;
            }
            const SceneMode mode = scene_ctl_.Mode();
            const float* mode_color = SceneModeColor();
            for (const SceneModeKey& key : kSceneModeKeys)
                SmtLed(KeyLedOf(key.key), key.color,
                       LoopsShown() ? 0.f : mode == key.mode ? 1.f : kFxOffLevel);

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
                    level = scene_flash_signal_.BlinkLevel(now);
                }
                else if (s == scene_refused_ && scene_refused_signal_.Active(now))
                {
                    color = red;
                    level = scene_refused_signal_.BlinkLevel(now);
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
                else if (s == scene_ctl_.Morphing() || s == scene_ctl_.DropSlot())
                {
                    // morphing to it: blinking on the FX clock's beats; red for what a drop
                    // lands on (#175: the build-up's B, white its A)
                    if (s == scene_ctl_.DropSlot())
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
                SmtLed(KeyLedOf(kSceneKeys[slot]), color, level);
            }
        }

        void DrawFxLeds(uint32_t now)
        {
            // knob LEDs: the selected FX's parameters in its colours, or the compressor's; a
            // knob the page doesn't use is dark. On an effect's page 2, knob 4's is its output's
            // VU meter (#117); a page just turned blinks its number on all four, white
            const size_t selected = fx_.Selected();
            const bool comp = selected == kFinSelected;
            const bool shifter = !comp && fx_.Mode(selected); // the phaser's second mode (#306)
            const float* const* colors = comp      ? kCompKnobColors
                                         : shifter ? kShifterKnobColors
                                                   : kFxSlots[selected].knob_colors;
            // the finalizer's meters run while its knobs show them
            const uint8_t meter = comp ? static_cast<uint8_t>(fx_.Module()) : Finalizer::kNoMeter;
            if (meter != fin_meter_)
            {
                fin_meter_ = meter;
                engine_->SetFinMeter(meter);
            }
            if (comp)
                FinMeters();
            // the page it blinks for, so another effect's page 1 isn't blinked over
            const bool flash = knob_page_flash_.Active(now) && fx_.Page() == knob_flash_page_;
            const float lit = knob_page_flash_.BlinkLevel(now);
            const FxParams& knobs = fx_.Knobs();
            for (size_t k = 0; k < kNumFxKnobs; k++)
            {
                float rgb[3] = {0.f, 0.f, 0.f};
                if (flash)
                    rgb[0] = rgb[1] = rgb[2] = lit;
                else if (comp)
                    FinKnobLed(k, now, knobs, rgb);
                else if (fx_.KnobUsed(k) && fx_.Page() && k == kVuKnob)
                    FxVu(selected, rgb);
                else if (shifter && fx_.ParamOf(k) == Phaser::DIRECTION)
                    Xfade3(kBipolarLow, white, kBipolarHigh, fx_.Knob(k), rgb); // down, both, up
                else if (fx_.KnobUsed(k))
                    KnobColor(knobs, fx_.ParamOf(k), fx_.Knob(k), colors, rgb);
                SetPthLedFloat(kFxKnobLeds[k], rgb[0], rgb[1], rgb[2]);
            }

            for (size_t fx = 0; fx < kNumFx; fx++)
            {
                const float* color = fx_.Mode(fx) ? kShifterKeyColor : kFxSlots[fx].key_color;
                const float meter = FxMeter(fx);
                // a mode switched: at full in the new colour, or a flash if it's on (#306)
                const bool mode_flash = fx == selected && mode_flash_.Active(now);
                float level = mode_flash ? 1.f : kFxOffLevel;
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
                if (fx == selected && (select_flash_.Active(now) || (mode_flash && fx_.IsOn(fx))))
                    Flash(rgb);
                SetSmtLedFloat(KeyLedOf(kFxSlots[fx].key), rgb[0], rgb[1], rgb[2]);
            }

            // the finalizer's key: the module's colour, its gain reduction a pulse from
            // kFinFloor up to full, dim while it's bypassed; turning red while the safety
            // limiter works, the one thing that sets the level by itself; a select flashes
            const float reduced = -engine_->GetFinReduction() / kFinMeterDb;
            // a module just picked (SHIFT + the key twice): at full in its colour a moment
            const float level = comp && mode_flash_.Active(now) ? 1.f
                                : fx_.Bypassed()               ? kFxOffLevel
                                               : kFinFloor + (1.f - kFinFloor) * fclamp(reduced, 0.f, 1.f);
            const float* fin_color = kFinColors[fx_.Module()];
            const float lim_db = -20.f * log10f(fmaxf(engine_->TakeLimiterGain(), 1e-6f));
            const float lim = fclamp((lim_db - kLimStartDb) / (kLimFullDb - kLimStartDb), 0.f, 1.f);
            if (lim >= lim_shown_ || now - lim_at_ > kLimHoldMs)
            {
                lim_shown_ = lim;
                lim_at_ = now;
            }
            float rgb[3];
            for (int c = 0; c < 3; c++)
                rgb[c] = level * fin_color[c] + lim_shown_ * (red[c] - level * fin_color[c]);
            if (comp && select_flash_.Active(now))
                Flash(rgb);
            if (master_refused_.Active(now))
                SmtLed(kCompKeyLed, red, master_refused_.BlinkLevel(now));
            else
                SetSmtLedFloat(kCompKeyLed, rgb[0], rgb[1], rgb[2]);
        }

        /** The finalizer's meters, falling back slowly as the input's: from the engine's peaks */
        void FinMeters()
        {
            Finalizer& fin = engine_->Fin();
            fin_level_[0] = fmaxf(fin.TakeInputPeak(), fin_level_[0] * kInMeterFall);
            for (size_t b = 0; b < kFinBands; b++)
                fin_level_[1 + b] = fmaxf(fin.TakeBandPeak(b), fin_level_[1 + b] * kInMeterFall);
            fin_lim_ = fminf(fin.TakeLimiterReduction(), fin_lim_ * kInMeterFall);
        }

        /** A finalizer knob's LED: a muted band's nearly dark (a flash as it toggles), a knob
         *  just turned its value, else the module's meter there (#345): Level's knob 1 the
         *  input, 4 the limiter's reduction; EQ's the bands' levels after it; Comp's their
         *  reductions; Width's their correlations; the rest their values */
        void FinKnobLed(size_t k, uint32_t now, const FxParams& knobs, float* rgb)
        {
            if (!fx_.KnobUsed(k))
                return;
            const size_t m = fx_.Module();
            const float* color = kFinColors[m];
            const bool band = m != kFinLevel && k < kFinBands;
            if (band && mute_flash_[k].Active(now))
            {
                rgb[0] = rgb[1] = rgb[2] = 1.f;
                return;
            }
            if (band && fx_.Muted(k))
            {
                for (int c = 0; c < 3; c++)
                    rgb[c] = kFinMutedLevel * color[c];
                return;
            }
            const bool meter = band || (m == kFinLevel && (k == 0 || k == kNumFxKnobs - 1));
            if (!meter || now - fin_turned_[k] < kFinValueMs)
            {
                KnobColor(knobs, fx_.ParamOf(k), fx_.Knob(k), kCompKnobColors, rgb);
                return;
            }
            Finalizer& fin = engine_->Fin();
            float t, gr;
            switch (m)
            {
            case Finalizer::WIDTH:
            {
                static const float* const kCorr[4] = {green, yellow, pink, red};
                const float* c = kCorr[Finalizer::CorrelationLed(fin.BandCorrelation(k))];
                rgb[0] = c[0];
                rgb[1] = c[1];
                rgb[2] = c[2];
                return;
            }
            case Finalizer::COMP:
                gr = fin.BandReduction(k);
                break;
            case Finalizer::EQ:
            default:
                if (m == kFinLevel && k != 0)
                {
                    gr = fin_lim_;
                    break;
                }
                // a level: dim to green to yellow to pink, from kInMeterFloorDb to the reference
                t = fclamp(1.f - 20.f * log10f(fmaxf(fin_level_[m == kFinLevel ? 0 : 1 + k], 1e-6f)) / kInMeterFloorDb, 0.f, 1.f);
                Xfade4(kVuDim, green, yellow, pink, t, rgb);
                return;
            }
            // a reduction: the module's colour, from the meter's floor up to full
            const float level = kVuDim[0] + (1.f - kVuDim[0]) * fclamp(-gr / kFinMeterDb, 0.f, 1.f);
            for (int c = 0; c < 3; c++)
                rgb[c] = level * color[c];
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
                loop[0] = loop_refused_.BlinkLevel(now);
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
                level = log_signal_.BlinkLevel(now);
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
         *  while SHIFT holds a scene sequence: the crossfader's, loop or not, or nothing
         *  (SceneControls::TransportTurned) */
        void TransportTurned(int16_t turns)
        {
            if (Shift())
            {
                bool scenes;
                {
                    ScopedIrqBlocker irq;
                    scenes = scene_ctl_.TransportTurned(turns, System::GetNow());
                    if (scene_ctl_.HandFader())
                    {
                        hand_fader_ = true;
#if !FRIZZ_BENCH
                        loops_.FaderTook();
#endif
                    }
                }
                if (scenes)
                {
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
        LedSignal mode_flash_;   // on its key after its mode switched (#306)
        bool tap_fin_ = false;   // SHIFT + key 15 last in this hold: again, its next module
        size_t tap_fx_ = kNumFx; // SHIFT + this key last in this hold, nothing since: the key
                                 // again switches its mode (kFxModes); kNumFx: none
        LedSignal mute_flash_[kFinBands]; // a finalizer knob as its band (un)mutes (#347)
        uint32_t fin_turned_[kNumFxKnobs]; // when each knob last turned (FinKnobLed); the page
                                           // is static, so these start at 0
        float fin_level_[1 + kFinBands];   // the finalizer's input and bands' meters, falling
        float fin_lim_ = 0.f;                   // and its limiter's reduction, dB, rising back
        uint8_t fin_meter_ = Finalizer::kNoMeter; // the module whose meters the engine runs
        uint8_t fin_preset_ = kNoFinPreset;       // the finalizer's preset recalled last
        static const uint8_t kNoFinPreset = 0xFF;
        bool fin_down_ = false;                   // key 15 held (FinPresetsShown)
        FinPresets fin_presets_;                  // their SAVE / COPY / DELETE
        int fin_flash_ = kNoScene;                // the preset written last, flashing
        bool fin_flash_waiting_ = false, fin_flash_ok_ = false;
        LedSignal fin_flash_signal_;
        int fin_refused_ = kNoScene;              // a scene key a preset refused
        LedSignal fin_refused_signal_;
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
        bool usb_host_ = false, usb_audio_ = true; // as Remote last saw them (MidiControl::LetGo)
        uint32_t gain_turned_ = 0;  // the input gain last set
        float in_meter_ = 0.f;      // the input's meter, full scale 1
        uint32_t in_red_at_ = 0;    // the input last reached the loop's ceiling
        bool hand_fader_ = false;   // the transport knob has had the crossfader since SHIFT
                                    // went down (TransportTurned)
        LedSignal page_flash_;      // a page picked: its number in blinks
        LedSignal knob_page_flash_; // the FX knobs' page turned: its number in blinks
        size_t knob_flash_page_ = 0; // the page knob_page_flash_ blinks for

        volatile bool loops_shown_ = false; // the Loops page or the Cubbi page (#70, loops_.Cubbi())
#if !FRIZZ_BENCH
        LoopControls loops_;
        int loop_fed_ = LoopControls::kNone; // a loop switched to, for MIDI feedback (#259)
        LedSignal loop_refused_key_;  // a loop refused, on its key
        size_t loop_refused_slot_ = 0;
        LedSignal bank_refused_;      // no bank further, on its key (both: no loop on the page)
        uint16_t bank_refused_key_ = 0;
        uint32_t tap_up_ = 0;         // SHIFT's last clean tap let go
        bool tap_one_ = false;        // one clean tap, the next may make it two
        bool tap_two_ = false;        // this press is the second
        bool tap_clean_ = false;      // nothing else since SHIFT went down
#endif
    };


} // namespace chompi

#pragma GCC pop_options
