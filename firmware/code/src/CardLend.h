/** @file CardLend.h
 *  @brief The card lent to the computer as a USB drive while FRIZZ plays on: usb-audio-poc,
 *  part 2 (#196), an experiment, not merged. From lost-tty/CHOMPI's TAPE (MIT, commit ab95f98:
 *  its card as a USB drive), with FRIZZ's own gesture and card users.
 *
 *  On the settings page, SHIFT + LOOP held 2 s with a computer on USB lends the card (LOOP
 *  blue on the settings page while it is); again, an eject on the computer, or the USB gone
 *  (unplugged, the computer asleep) takes it back. Refused, LOOP blinking red 3 times, without
 *  a computer, while LOOP records, a loop loads or the card is being written (a scene, the
 *  settings, a bug report). The developers' SysEx asks the same (kCmdDev's USB).
 *
 *  While it's lent FRIZZ doesn't touch the card: no saves (they wait until it's back), no loop
 *  loads, no bug report (one asked for fails). The loop in RAM and the effects play on. Taken
 *  back, the card is mounted afresh and the loops are scanned again (the computer may have
 *  changed them); the scenes and the settings stay as they are in RAM.
 *
 *  MainLoop decides (chompi_main.cpp); this holds what the settings page shows and asks.
 */
#pragma once
#include <stdint.h>
#include "LedSignal.h"

namespace cardlend
{
struct State
{
    volatile bool lent = false;  // the computer has the card
    volatile bool asked = false; // the gesture (or the SysEx): lend it, or take it back
    LedSignal refused;           // LOOP's 3 red blinks
};

inline State& Get()
{
    static State state;
    return state;
}
} // namespace cardlend
