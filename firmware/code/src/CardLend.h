/** @file CardLend.h
 *  @brief The card lent to the computer as a USB drive while FRIZZ plays on (#196). The idea
 *  and the drive are lost-tty/CHOMPI's TAPE (MIT, commit ab95f98); the gesture and what FRIZZ
 *  holds back meanwhile are FRIZZ's.
 *
 *  On the settings page, SHIFT + LOOP held 2 s with a computer on USB lends the card (LOOP
 *  blue there while it's lent; SHIFT's LED stays SHIFT's). The same gesture again, an eject
 *  on the computer, USB gone (unplugged, the computer asleep) or a restart over MIDI (before
 *  it resets, chompi_main.cpp) takes it back. Refused, LOOP
 *  blinking red 3 times: without a computer, while LOOP records, a loop loads or the card is
 *  being written (a scene, the settings, a bug report). The developers' SysEx asks the same
 *  (kCmdDev's USB); kCmdDev's LOOPS that start something are refused while it's lent.
 *
 *  While it's lent FRIZZ doesn't touch the card: no saves (they wait until it's back), no loop
 *  loads, no bug report (one asked for fails). The loop in RAM and the effects play on. Taken
 *  back, the card is mounted afresh and the loops are scanned again (the computer may have
 *  changed them), and the scenes and the settings the computer changed are read again, as at
 *  boot, unless the device changed its own meanwhile, which then wins (SceneStore::Reread;
 *  #291).
 *
 *  MainLoop decides (chompi_main.cpp's CardLendProcess); this holds what the settings page
 *  shows and asks.
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
