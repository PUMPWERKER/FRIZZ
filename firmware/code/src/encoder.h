#pragma once
#include "daisy_core.h"
#include "hid/switch.h"

namespace chompi
{
/**
    @brief Generic Class for handling Quadrature Encoders \n
    Inspired/influenced by Mutable Instruments (pichenettes) Encoder classes
    @author Electrosmith
    @date December 2019
    @ingroup controls
*/
class ChompiEncoder
{
  public:
    ChompiEncoder() {}
    ~ChompiEncoder() {}

    /** Initializes the encoder with the specified hardware pins */
    void Init(dsy_gpio_pin a, dsy_gpio_pin b, dsy_gpio_pin click);
    /** Called at update_rate to debounce and handle timing for the switch.
     * In order for events not to be missed, its important that the Edge/Pressed checks be made at the same rate as the debounce function is being called.
     */
    void Debounce();

    /**
      Just does the A and B based on input states
      For use when your A and B lines are on a SR
    */
    void Debounce(bool a_state, bool b_state);

    /** Returns +1 if the encoder was turned clockwise, -1 if it was turned counter-clockwise, or 0 if it was not just turned. */
    inline int32_t Increment() const { return updated_ ? inc_ : 0; }

    /** Returns true if the encoder was just pressed. */
    inline bool RisingEdge() const { return sw_.RisingEdge(); }

    /** Returns true if the encoder was just released. */
    inline bool FallingEdge() const { return sw_.FallingEdge(); }

  private:
    /** Debounce's, both kinds: the lines' new samples, and how many a fall takes */
    void Update(bool a_state, bool b_state, uint8_t mask) __attribute__((noinline));

    uint32_t last_update_;
    bool     updated_;
    daisy::Switch   sw_;
    bool     click_ = false; // the switch has a pin: without one, it's never read
    dsy_gpio hw_a_, hw_b_;
    uint8_t  a_, b_;
    bool     armed_ = false; // A and B were both high since the last count (Update)
    int32_t  inc_;
};
} // namespace chompi
