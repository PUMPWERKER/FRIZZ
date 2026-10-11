/** @file ui.h
 *  @brief UI built on libDaisy's daisy::UI page system. Each screen of the UI (the play page,
 *  the boot glow, the rainbow) is a UiPage subclass
 *
 *  Pages are opened and closed here and the UI events get generated and sent to
 *  the active page.
 */
#pragma once
#include "hardware.h"
#include "NormalPage.h"
#include "BootPage.h"
#include "RainbowWavePage.h"
#include "passthroughEngine.h"
#include "EventLog.h"
#include "MidiControl.h"

/** The pages draw their LEDs themselves (fill_led_data, temp_led_stuff.h), so the canvas's
 *  clear and flush that libDaisy's UI framework asks for do nothing */
inline void FlushLeds(const daisy::UiCanvasDescriptor&) {}
inline void ClearLeds(const daisy::UiCanvasDescriptor&) {}

namespace chompi
{
    static const uint8_t encoder_map[6] = {1, 2, 3, 0, 4, 5};

    enum CanvasIds
    {
        canvasLedDisplay = 0,
        NUM_CANVASES
    };

    class UserInterface
    {
    public:
        void Init(PassthroughEngine *engine, Hardware *hw, SceneStore *scenes, EventLog *log,
                  MidiControl *midi)
        {
            hw_ = hw;
            log_ = log;
            midi_ = midi;

            /** Describe UI special controls - if any */
            daisy::UI::SpecialControlIds specialControlIds; /**< None here */
            /** Canvas Descriptor */
            daisy::UiCanvasDescriptor ledDisplayDescriptor;
            ledDisplayDescriptor.id_ = canvasLedDisplay;
            ledDisplayDescriptor.handle_ = nullptr;
            ledDisplayDescriptor.updateRateMs_ = 16; /**< ~60Hz */
            ledDisplayDescriptor.clearFunction_ = ClearLeds;
            ledDisplayDescriptor.flushFunction_ = FlushLeds;

            /** Init */
            ui.Init(event_queue,
                    specialControlIds,
                    {ledDisplayDescriptor},
                    canvasLedDisplay);

            normal_page_.Init(engine, hw_, scenes, log, midi);
            ui.OpenPage(normal_page_);

            boot_page_.Init();
            ui.OpenPage(boot_page_);
        }

        void RainbowWave()
        {
            ui.OpenPage(rainbow_page_);
        }
        inline bool InRainbows() { return rainbow_page_.IsActive(); }

        void StopBootAnimation()
        {
            ui.ClosePage(boot_page_);
        }

        // Translates raw debounced hardware state into daisy::UiEventQueue events
        // for whatever page is active, and logs each for a bug report (EventLog.h). A key
        // is down while the hand or MIDI holds it (MidiControl.h), so one held by both is
        // pressed once and let go once both have; only the hand's are logged here, MIDI's as
        // the messages that came.
        // The mode switch up (GetToggleState() false, or SysEx's kCmdSwitch) shows the settings
        // page: a key the panel presses then goes to it, as kSettingsKeyBase plus the key, and
        // its turns go nowhere. The panel is the hand and FRIZZ's SysEx keys and detents, so a
        // computer can play it as a hand would; MIDI's notes and CCs still play. A key belongs to the side it went down on until
        // it's let go, so flipping the switch while holding one neither lets go of it on the
        // play page (an FX punched in, PLAY or CHOMPI acting on their release) nor presses it
        // on the settings page.
        void GenerateEvents()
        {
            const uint8_t sw = midi_->Switch();
            settings_ = sw == 0 ? !hw_->GetToggleState() : sw == 2;
            normal_page_.ShowSettings(settings_);

            for (int i = 0; i < static_cast<int>(Hardware::SwId::SR_LAST); i++)
            {
                // the transport's switch comes from its encoder below; the mode switch isn't a
                // key
                if (i == ENC_5_SW || i == static_cast<int>(Hardware::SwId::SW_TOG))
                    continue;
                else if (hw_->button_sr.FallingEdge(i))
                    Hand(i, false);
                else if (hw_->button_sr.RisingEdge(i))
                    Hand(i, true);
            }

            if (hw_->enc[4].FallingEdge())
                Hand(ENC_5_SW, false);
            else if (hw_->enc[4].RisingEdge())
                Hand(ENC_5_SW, true);
            log_->Toggle(hw_->GetToggleState());

            const uint64_t panel = hand_ | midi_->PanelKeys();
            if (settings_)
                settings_keys_ |= panel & ~panel_;
            panel_ = panel;
            Tell((panel & ~settings_keys_) | midi_->TakeKeys(), told_, 0);
            Tell(panel & settings_keys_, told_settings_, kSettingsKeyBase);
            settings_keys_ &= panel;

            // encoder_map remaps the encoders' wiring order to the knobs' order; one event per
            // detent
            for (int i = 0; i < static_cast<int>(Hardware::EncoderId::ENC_LAST); i++)
            {
                int inc = hw_->enc[i].Increment();
                if (inc == 1 || inc == -1)
                {
                    if (!settings_)
                        event_queue.AddEncoderTurned(encoder_map[i], inc, 0);
                    log_->Add(EventLog::TURN, i + 1, inc);
                }
            }
            // FRIZZ's SysEx detents, in the knobs' order already, one a block, as the hand's
            for (uint16_t knob = 0; knob < midimap::kNumKnobs; knob++)
            {
                const int panel_turn = midi_->TakePanelTurn(knob);
                if (panel_turn && !settings_)
                    event_queue.AddEncoderTurned(knob, static_cast<int16_t>(panel_turn), 0);
            }
        }

        /** Closes the finished rainbow page, then dispatches the events and draws. Runs in
         *  one context at a time (chompi_main.cpp), so the page stack is only changed here;
         *  GenerateEvents only adds to the IRQ-safe event queue. What came over MIDI for the
         *  play page waits for MainLoop (from_main), which may answer over USB */
        void DoEvents(bool from_main = true)
        {
            if(rainbow_page_.IsClosable() && rainbow_page_.IsActive())
            {
                ui.ClosePage(rainbow_page_);
                normal_page_.ResetSmtLeds();
            }

            if (from_main)
                normal_page_.Remote();
            ui.Process();
        }

    /** Before a restart: true once the play page has nothing left for the card */
        bool MasterSettled() { return normal_page_.MasterSettled(); }

    private:
        /** A key the hand pressed or let go, as its debouncing saw it */
        void Hand(int key, bool down)
        {
            if (down)
                hand_ |= 1ull << key;
            else
                hand_ &= ~(1ull << key);
            log_->Add(EventLog::KEY, key, down ? 1 : 0);
        }

        /** The keys down on one side, against what it was told: a press or a release for each
         *  that changed, as base plus the key */
        void Tell(uint64_t down, uint64_t& told, uint16_t base)
        {
            for (uint64_t changed = down ^ told; changed; changed &= changed - 1)
            {
                const int key = __builtin_ctzll(changed);
                if ((down >> key) & 1)
                    event_queue.AddButtonPressed(base + key, 1);
                else
                    event_queue.AddButtonReleased(base + key);
            }
            told = down;
        }

        MidiControl *midi_ = nullptr;
        uint64_t hand_ = 0; // the keys the hand holds, by Hardware::SwId
        uint64_t told_ = 0; // the keys the play page was told are down
        bool settings_ = false;         // the mode switch is up: the settings page
        uint64_t panel_ = 0;            // the panel's keys down: the hand's and SysEx's
        uint64_t settings_keys_ = 0;    // those that went down on the settings page
        uint64_t told_settings_ = 0;    // the keys it was told are down

        BootPage boot_page_;
        NormalPage normal_page_;
        RainbowPage rainbow_page_;
        daisy::UiEventQueue event_queue;
        daisy::UI ui;
        Hardware *hw_;
        EventLog *log_;
    };

} // namespace chompi
