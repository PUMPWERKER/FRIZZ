/** @file hardware.h
 *  @brief Hardware abstraction layer for the CHOMPI synth.
 */
#pragma once
#include "daisy_seed.h"
#include "encoder.h"
#include "temp_led_stuff.h"

#define ENC_5_SW 4 // this one weird switch isn't on the SR

#define NO_BATT false

using namespace daisy;

namespace chompi
{

    /** @brief Hardware support class for the CHOMPI hardware */
// The MP2722 battery chip's status registers, received by DMA (Hardware::MpReadAll). The D-cache
// would hide the DMA's writes from a buffer in ordinary RAM, so it lives in D2 RAM, uncached
// (libDaisy's MPU setup), like the LED data in temp_led_stuff.h. This header is only included
// by chompi_main.cpp
uint8_t DMA_BUFFER_MEM_SECTION mp_dma_buff[6];

    class Hardware
    {
    public:
        /* Indices for encoder access based on hardware reference name */
        enum class EncoderId : int
        {
            SW1,
            SW2,
            SW3,
            SW4,
            SW5,
            SW6,
            ENC_LAST,
        };

        /** Switch reference for the 4021 chain
         */
        enum class SwId
        {
            /** SR 1 (start of chain) (U5) */
            ENC_1_SW, /**< U5 1 */
            ENC_2_SW, /**< U5 2 */
            ENC_3_SW, /**< U5 3 */
            ENC_4_SW, /**< U5 4 */
            NC_6,     /**< U5 5 */
            KEY_26,   /**< U5 6 */
            SW_TOG,   /**< U5 7 */
            KEY_16,   /**< U5 8 */
            /** SR 2 (start of chain) (U7) */
            KEY_2,  /**< U7 1 */
            KEY_3,  /**< U7 2 */
            KEY_4,  /**< U7 3 */
            KEY_5,  /**< U7 4 */
            KEY_17, /**< U7 5 */
            KEY_18, /**< U7 6 */
            KEY_19, /**< U7 7 */
            KEY_1,  /**< U7 8 */
            /** SR 3 (start of chain) (U8) */
            KEY_6,  /**< U8 1 */
            KEY_7,  /**< U8 2 */
            KEY_8,  /**< U8 3 */
            KEY_9,  /**< U8 4 */
            KEY_10, /**< U8 5 */
            KEY_20, /**< U8 6 */
            KEY_21, /**< U8 7 */
            KEY_22, /**< U8 8 */
            /** SR 4 (start of chain) (U9) */
            KEY_11, /**< U9 1 */
            KEY_12, /**< U9 2 */
            KEY_13, /**< U9 3 */
            KEY_14, /**< U9 4 */
            KEY_15, /**< U9 5 */
            KEY_23, /**< U9 6 */
            KEY_24, /**< U9 7 */
            KEY_25, /**< U9 8 */
            /** SR 5 (start of chain) (U10) */
            ENC_6_SW, /**< U10 1 */
            KEY_27,   /**< U10 2 */
            KEY_28,   /**< U10 3 */
            NC_1,     /**< U10 4 */
            NC_2,     /**< U10 5 */
            NC_3,     /**< U10 6 */
            NC_4,     /**< U10 7 */
            NC_5,     /**< U10 8 */
            SR_LAST,
        };

        /** Initialize the hardware */
        Hardware() {}
        void Init()
        {
            /** Daisy Seed Initialization */
            seed.Init(true);

            /** secondary audio */
            /** Configure the SAI2 peripheral for the secondary codec.
             *  CHOMPI has two stereo output pairs (headphone + master, see the
             *  channel breakdown comment above AudioCallback in chompi_main.cpp) driven
             *  off two SAI peripherals in sync: the Seed's own built-in SAI1 codec
             *  plus this second external one on SAI2. */
            SaiHandle::Config external_sai_cfg;
            external_sai_cfg.periph = SaiHandle::Config::Peripheral::SAI_2;
            external_sai_cfg.sr = SaiHandle::Config::SampleRate::SAI_48KHZ;
            external_sai_cfg.bit_depth = SaiHandle::Config::BitDepth::SAI_24BIT;
            external_sai_cfg.a_sync = SaiHandle::Config::Sync::SLAVE;
            external_sai_cfg.b_sync = SaiHandle::Config::Sync::MASTER;
            external_sai_cfg.a_dir = SaiHandle::Config::Direction::TRANSMIT;
            external_sai_cfg.b_dir = SaiHandle::Config::Direction::RECEIVE;
            external_sai_cfg.pin_config.fs = seed::D27;
            external_sai_cfg.pin_config.mclk = seed::D24;
            external_sai_cfg.pin_config.sck = seed::D28;
            external_sai_cfg.pin_config.sb = seed::D25;
            external_sai_cfg.pin_config.sa = seed::D26;

            /** Initialize the SAI new handle */
            external_sai_handle.Init(external_sai_cfg);

            AudioHandle::Config audio_cfg;
            audio_cfg.blocksize = 24;
            audio_cfg.samplerate = SaiHandle::Config::SampleRate::SAI_48KHZ;
            audio_cfg.postgain = 1.0f; /*< TODO: we may want to fine tune this */

            seed.audio_handle.Init(audio_cfg, seed.AudioSaiHandle(), external_sai_handle);

            /** MP2722 Power Comms */
            I2CHandle::Config i2c_conf;
            i2c_conf.mode = I2CHandle::Config::Mode::I2C_MASTER;
            i2c_conf.periph = I2CHandle::Config::Peripheral::I2C_1;
            i2c_conf.speed = I2CHandle::Config::Speed::I2C_100KHZ;
            i2c_conf.address = 0x3F;
            i2c_conf.pin_config.scl = seed::D11;
            i2c_conf.pin_config.sda = seed::D12;

            i2c.Init(i2c_conf);

            // 2722 Interrupt, USB Switch Control, input jack detection
            mpc_int.Init(seed::D31, GPIO::Mode::INPUT, GPIO::Pull::NOPULL);     // move this to be an actual interrupt?
            usb_sw.Init(seed::D32, GPIO::Mode::OUTPUT, GPIO::Pull::NOPULL);     // pulldown in hw
            jack_detect.Init(seed::D21, GPIO::Mode::INPUT, GPIO::Pull::NOPULL); // pullup in hw

            /** Encoders */
            enc[(int)EncoderId::SW1].Init(Pin(), Pin(), Pin());
            enc[(int)EncoderId::SW2].Init(Pin(), Pin(), Pin());
            enc[(int)EncoderId::SW3].Init(Pin(), Pin(), Pin());
            enc[(int)EncoderId::SW4].Init(Pin(), Pin(), Pin());
            enc[(int)EncoderId::SW5].Init(seed::D0, seed::D20, seed::D10);
            enc[(int)EncoderId::SW6].Init(seed::D15, seed::D17, Pin());

            /** Switches 5x CD4021 in series
             *  See SwId for mapping. A 4021 is a parallel-in/serial-out shift register.
             *  Each chip has 8 button states in parallel then all 5
             *  chained chips shift their 40 bits out on one data line
             *  clocked by clk so 40 buttons cost 3 GPIO pins instead of 40.
             *  libDaisy has a class for this.
             */
            ShiftRegister4021<5, 1>::Config button_sr_cfg;
            button_sr_cfg.clk = seed::D8;
            button_sr_cfg.latch = seed::D7;
            button_sr_cfg.data[0] = seed::D9;
            button_sr_cfg.dbc_size = 7;
            button_sr.Init(button_sr_cfg);

            ShiftRegister4021<1, 1>::Config encoder_sr_cfg;
            encoder_sr_cfg.clk = seed::D22;
            encoder_sr_cfg.latch = seed::D23;
            encoder_sr_cfg.data[0] = seed::D19;
            encoder_sr_cfg.dbc_size = 50; // we're not actually using this
            encoder_sr.Init(encoder_sr_cfg);

            // the first level check a second after boot, not after the usual 30s
            batt_level_checkt = System::GetNow() - kBattLevelCheckMs + 1000;
        }

        enum BatteryLevel {
            FULL, // on the charger: charging or full
            HIGH, // > 3V3, not fully charged
            MEDIUM, // < 3V3
            LOW, // < 3V, the lockout's threshold: shuts down soon unless it's charging
        };

        static const uint32_t kBattLevelCheckMs = 30000;
        BatteryLevel batt_level = MEDIUM;

        inline BatteryLevel GetBatteryLevel()
        {
            // the cable, as the lockout reads it, so it shows as soon as it's in or out
            if (vin_gd_bounce != 0x00)
                return BatteryLevel::FULL;
            if (batt_level != BatteryLevel::FULL && batt_low_bounce == 0xff)
                return BatteryLevel::LOW;
            return batt_level;
        }

        /** Debounce registers for noisy MP2722. It tends to fluctuate a bit on the
         *  thresholds between battery levels */
        uint8_t batt_low_bounce = 0;
        uint8_t vin_gd_bounce = 0xff;

        uint8_t legacy_cable_bounce = 0;
        uint8_t iindpm_stat_bounce = 0;

        /* Ported from TEMPO hardware.h: non-blocking battery check. Reads the battery IC without
        blocking so MIDI and UI don't lag */
        uint8_t batt_check_state = 0;
        void BMCMediumBattCheck()
        {
            if(batt_level == BatteryLevel::FULL)
                return;

            // its read is its own (level_read_), and the lockout doesn't read meanwhile, so the
            // 3V3 reading can't be overwritten before it's used, nor reach the lockout
            uint32_t now = System::GetNow();
            if (batt_check_state == 0 && now - batt_level_checkt > kBattLevelCheckMs) {
                MpWrite(0x0c, 0B01011101); // set BATT_LOW to 3V3
                batt_level_checkt = now;
                batt_check_state = 1;
            }
            else if (batt_check_state == 1 && now - batt_level_checkt > 30) {
                level_read_ = true;
                MpReadAll();
                batt_level_checkt = now;
                batt_check_state = 2;
            }
            else if (batt_check_state == 2 && (!level_read_ || now - batt_level_checkt > 200)) {
                // read, or given up on (it failed): back to 3V either way
                MpWrite(0x0c, 0B01010001); // set BATT_LOW to 3V
                if (!level_read_ && !read_error)
                    batt_level = level_low_ ? BatteryLevel::MEDIUM : BatteryLevel::HIGH;
                level_read_ = false;
                batt_level_checkt = now;
                batt_check_state = 0;
            }
        }

        /** Before sleeping on a legacy cable with a low battery: all LEDs off */
        void LedsOff()
        {
            PthLedsOff();
            SmtLedsOff();

            // ========   send the data   =========
            fill_led_data();
        }

        uint32_t batt_level_checkt;
        bool plugged_ = true; // the cable at the last check, as vin_gd_bounce starts
        /** Called periodically from MainLoop() (see chompi_main.cpp) to protect the
         *  battery from running on too low battery. */
        void LowBatteryLockoutCheck()
        {
            #if NO_BATT
                return;
            #endif 

            // not during the level check, whose threshold is raised to 3V3 (BMCMediumBattCheck)
            if(batt_check_state == 0)
                MpReadAll();

            // the cable just pulled: the level read a second on, not up to 30 s later (what
            // the last check read while charging was the charger's voltage)
            const bool plugged = vin_gd_bounce != 0x00;
            if(plugged_ && !plugged && batt_check_state == 0)
                batt_level_checkt = System::GetNow() - kBattLevelCheckMs + 1000;
            plugged_ = plugged;

            if(batt_low_bounce == 0xff && vin_gd_bounce == 0x00) // unplugged and low battery
            {
                // 15 s amber warning before shutdown.
                // Deliberately BLOCKING (audio keeps running - it is interrupt-driven); flashes
                // all PTH LEDs at 250 ms, re-checks battery each cycle, aborts if power returns.
                uint32_t shutdown_time_ = System::GetNow();
                uint32_t last_update = shutdown_time_;

                while(System::GetNow() - shutdown_time_ < 15000)
                {
                    uint32_t now = System::GetNow();
                    if(now - last_update >= 250)
                    {
                        last_update = now;

                        MpReadAll(); // re-check battery status

                        if(!(batt_low_bounce == 0xff && vin_gd_bounce == 0x00))
                            return; // break out early if battery status changed

                        bool led_on = (now / 250) % 2 == 0;
                        for(size_t i = 0; i < kPthLeds; ++i)
                        {
                            if(led_on)
                                SetPthLedFloat(i, 1.f, .95f, .05f);
                            else
                                SetPthLedFloat(i, 0.f, 0.f, 0.f);
                        }

                        fill_led_data();
                    }
                }

                MpWrite(0x08, 0B10111111); // SHIPPING MODE
            }
            
            // plugged into low current source with low batt: sleeps for good, as the stock
            // firmware does. Nothing in here reads the battery again, and STOP stops the clocks,
            // which a wake-up wouldn't restore; the charger IC goes on charging by itself, and
            // the power switch starts FRIZZ again
            while(batt_low_bounce == 0xff && (legacy_cable_bounce == 0xff || iindpm_stat_bounce == 0xff))
            {
                LedsOff();
                System::Delay(100);
                HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON , PWR_STOPENTRY_WFI);
            }

            // check for medium state every 30 seconds (gate lives inside BMCMediumBattCheck now)
            BMCMediumBattCheck();
        }

        void MpWrite(uint8_t reg, uint8_t data)
        {
            #if NO_BATT
                return;
            #endif

            uint16_t address = 0x3F;

            uint8_t tx_buff[] = {reg, data};
            i2c.TransmitBlocking(address, tx_buff, 2, 200);
        }

        void MpReadAll()
        {
            #if NO_BATT
                return;
            #endif 

            uint16_t address = 0x3F;

            uint8_t tx_buff[] = {0x11};
            i2c.TransmitBlocking(address, tx_buff, 1, 200);

            size_t buff_size = 0x06;
            i2c.ReceiveDma(address | 0B10000000, mp_buff_, buff_size, batteryCallback, this);
        }

        // DMA-complete callback for the MP2722 status
        static void batteryCallback(void *context, daisy::I2CHandle::Result result)
        {
            Hardware *self = static_cast<Hardware*>(context);
            if(result != daisy::I2CHandle::Result::OK)
            {
                // nothing new: the debouncing keeps what it had, and a waiting reader goes on
                self->level_read_ = false;
                self->read_error = true;
                return;
            }
            self->read_error = false;

            uint8_t *buff = self->mp_buff_;

            const bool iindpm_stat = buff[0] & 1;
            const bool vin_gd = (buff[1] >> 6) & 1;
            const bool legacy_cable = (buff[1] >> 4) & 1;
            const bool batt_low_stat = (buff[5] >> 4) & 1;

            if(self->level_read_)
            {
                self->level_low_ = batt_low_stat;
                self->level_read_ = false;
                return;
            }

            self->batt_low_bounce = (self->batt_low_bounce << 1) | batt_low_stat;
            self->vin_gd_bounce   = (self->vin_gd_bounce << 1) | vin_gd;
            self->legacy_cable_bounce = (self->legacy_cable_bounce << 1) | legacy_cable;
            self->iindpm_stat_bounce  = (self->iindpm_stat_bounce << 1) | iindpm_stat;

            const uint8_t chg_stat = (buff[2] >> 5) & 0B111;
            // full: until it isn't (the cable pulled), then HIGH until the next level check.
            // The stock firmware held FULL 20 minutes on; the cable shows by itself now
            // (GetBatteryLevel)
            if(chg_stat == 0B101)
                self->batt_level = Hardware::BatteryLevel::FULL;
            else if(self->batt_level == Hardware::BatteryLevel::FULL)
                self->batt_level = Hardware::BatteryLevel::HIGH;
        }

        // the MP2722's status, received by DMA: in the uncached D2 RAM (mp_dma_buff)
        uint8_t* const mp_buff_ = mp_dma_buff;
        volatile bool read_error = false; // the last read failed: mp_buff_ is older
        // the level check's own read (BMCMediumBattCheck): taken at the 3V3 threshold, so it
        // doesn't feed the lockout's debouncing
        volatile bool level_read_ = false;
        volatile bool level_low_ = false;

        bool GetToggleState()
        {
            return tog_state < 100;
        }

        uint32_t tog_state = 0;
    
        void ProcessAllControls() __attribute__((optimize("-O0")))
        {
            button_sr.Update();
            encoder_sr.Update();

            if(!button_sr.State(static_cast<int>(Hardware::SwId::SW_TOG)))
                tog_state++;
            else if(tog_state != 0)
                tog_state--;

            tog_state = tog_state > 200 ? 200 : tog_state;

            for (size_t i = 0; i < 4; i++)
            {
                const bool a_state = encoder_sr.RawState(i * 2);
                const bool b_state = encoder_sr.RawState(i * 2 + 1);
                enc[i].Debounce(a_state, b_state);
            }

            enc[4].Debounce();
            enc[5].Debounce();
        }

        void StartAudio(AudioHandle::AudioCallback cb)
        {
            seed.StartAudio(cb);
        }

        DaisySeed seed;

        ChompiEncoder enc[6];
        ShiftRegister4021<5, 1> button_sr;
        ShiftRegister4021<1, 1> encoder_sr;
        SaiHandle external_sai_handle;
        I2CHandle i2c; // comms w/ MP2722
        GPIO usb_sw, mpc_int, jack_detect; // mpc_int and jack_detect are set up, but not read
    private:
    };

    // the transport's switch takes the chain's unused slot (ui.h)
    static_assert(ENC_5_SW == static_cast<int>(Hardware::SwId::NC_6), "ENC_5_SW is NC_6's slot");

} // namespace chompi