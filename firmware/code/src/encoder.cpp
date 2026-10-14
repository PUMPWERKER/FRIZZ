#include "encoder.h"

using namespace chompi;

/** False for Pin(), which libDaisy uses for a pin that isn't there */
static bool Exists(dsy_gpio_pin p) { return p.port != DSY_GPIOX && p.pin < 16; }

void ChompiEncoder::Init(dsy_gpio_pin a, dsy_gpio_pin b, dsy_gpio_pin click)
{
    last_update_ = daisy::System::GetNow();
    updated_     = false;

    // Init GPIO for A, and B
    hw_a_.pin  = a;
    hw_a_.mode = DSY_GPIO_MODE_INPUT;
    hw_a_.pull = DSY_GPIO_PULLUP;
    hw_b_.pin  = b;
    hw_b_.mode = DSY_GPIO_MODE_INPUT;
    hw_b_.pull = DSY_GPIO_PULLUP;
    // a pin that isn't there (Pin(): the A and B on the shift register, no click) is left
    // alone: libDaisy's C GPIO calls don't check it, and would set up port NULL with a pin
    // read from past the end of their table
    if (Exists(a))
        dsy_gpio_init(&hw_a_);
    if (Exists(b))
        dsy_gpio_init(&hw_b_);
    // Default Initialization for Switch
    click_ = Exists(click);
    if (click_)
        sw_.Init(click);
    // Set initial states, etc.
    inc_ = 0;
    a_ = b_ = 0xff;
}

// Bitwise shifts in new reading of pins so that pin needs to consistently
// read one way or the other for a few samples before being treated as a state change
// this rejects noise/switch bouncing.
void ChompiEncoder::Debounce()
{
    // update no faster than 1kHz
    uint32_t now = daisy::System::GetNow();
    updated_     = false;

    if(now - last_update_ >= 1)
    {
        last_update_ = now;
        updated_     = true;

        // Shift Button states to debounce
        a_ = (a_ << 1) | dsy_gpio_read(&hw_a_);
        b_ = (b_ << 1) | dsy_gpio_read(&hw_b_);

        // infer increment direction
        inc_ = 0; // reset inc_ first
        if((a_ & 0x03) == 0x02 && (b_ & 0x03) == 0x00)
        {
            inc_ = 1;
        }
        else if((b_ & 0x03) == 0x02 && (a_ & 0x03) == 0x00)
        {
            inc_ = -1;
        }
    }

    // Debounce built-in switch
    if (click_)
        sw_.Debounce();
}

void ChompiEncoder::Debounce(bool a_state, bool b_state)
{
    
    // update no faster than 1kHz
    uint32_t now = daisy::System::GetNow();
    updated_     = false;

    if(now - last_update_ >= 1)
    {
        last_update_ = now;
        updated_     = true;

        // Shift Button states to debounce
        a_ = (a_ << 1) | a_state;
        b_ = (b_ << 1) | b_state;

        // infer increment direction
        inc_ = 0; // reset inc_ first
        if((a_ & 0x07) == 0x04 && (b_ & 0x03) == 0x00)
        {
            inc_ = 1;
        }
        else if((b_ & 0x07) == 0x04 && (a_ & 0x03) == 0x00)
        {
            inc_ = -1;
        }
    }
    // the switch isn't debounced here: on the shift register, its state comes from there
}

/** libDaisy's System::Init sets up its SPI handles; FRIZZ has no SPI (the LEDs are timer PWM, the
 *  shift registers bit-banged, the codec on I2C, the card on SDMMC). Defined here, the call
 *  keeps libDaisy's SPI driver and the HAL's out of the build: 5.7 KB of code, 2.9 KB of RAM */
extern "C" void dsy_spi_global_init() {}

#ifdef __arm__
/** Nothing in FRIZZ is allocated (no malloc, no new); the deleting destructors of libDaisy's UI
 *  pages still name operator delete, which would pull newlib's free and its locks */
void operator delete(void*) noexcept {}
void operator delete(void*, unsigned int) noexcept {}
#endif
