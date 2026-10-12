// usbmidi.cpp: libDaisy's USB MIDI out (code/libs/libDaisy/src/hid/usb_midi.cpp, compiled here
// as it is) against what a host makes of it. libDaisy packs each message into USB MIDI packets
// of 4 bytes, a code index number (CIN) first; the host's driver (ALSA's snd-usb-audio, and the
// USB MIDI 1.0 spec) takes as many bytes from each as its CIN says. Every message FRIZZ sends
// over USB (MidiOut.h's clock and transport, MIDI feedback's CCs, the answers' SysEx) must
// arrive once and whole. The virtual CHOMPI can't show this: its USB passes raw bytes, the
// packets left out (twin/host/hid/usb_midi.h). The libdaisy.a FRIZZ links is built from this
// source (THIRD_PARTY.md). Run by unit.sh usbmidi.
// unit includes: usbmidi ../code/libs/libDaisy/src ../code/libs/libDaisy/src/sys
#include "../code/libs/libDaisy/src/hid/usb_midi.cpp"
#include "check.h"
#include <string>
#include <vector>

uint8_t usbd_mode = USBD_MODE_CDC;

// what usb_midi.cpp uses of the rest of libDaisy: the transfers kept, nothing waits
static std::vector<uint8_t> sent;
void System::Delay(uint32_t) {}
void System::DelayUs(uint32_t) {}
void UsbHandle::Init(UsbPeriph) {}
void UsbHandle::Reset() {}
void UsbHandle::SetReceiveCallback(ReceiveCallback, UsbPeriph) {}
UsbHandle::Result UsbHandle::TransmitInternal(uint8_t* buff, size_t size)
{
    sent.insert(sent.end(), buff, buff + size);
    return Result::OK;
}
UsbHandle::Result UsbHandle::TransmitExternal(uint8_t* buff, size_t size) { return TransmitInternal(buff, size); }

namespace
{
MidiUsbTransport usb;

/** The bytes a host reads from the packets sent: each packet's CIN says how many (0 and 1
 *  reserved: none) */
std::vector<uint8_t> HostReads(const std::vector<uint8_t>& packets)
{
    static const uint8_t kLength[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    std::vector<uint8_t> got;
    for (size_t i = 0; i + 3 < packets.size(); i += 4)
        got.insert(got.end(), packets.begin() + i + 1, packets.begin() + i + 1 + kLength[packets[i] & 0x0F]);
    return got;
}

std::string Hex(const std::vector<uint8_t>& v)
{
    std::string s;
    char b[4];
    for (uint8_t x : v)
    {
        snprintf(b, sizeof b, "%s%02x", s.empty() ? "" : " ", x);
        s += b;
    }
    return s;
}

/** msg sent as one transfer: the host reads it once, whole */
void Once(const char* what, std::vector<uint8_t> msg)
{
    sent.clear();
    usb.Tx(msg.data(), msg.size());
    const std::vector<uint8_t> got = HostReads(sent);
    const std::string text = std::string(what) + ": " + Hex(msg) + " reads as " + (got.empty() ? "nothing" : Hex(got));
    Check(got == msg && sent.size() % 4 == 0, text.c_str());
}
} // namespace

int main()
{
    MidiUsbTransport::Config config;
    usb.Init(config);
    // MidiOut.h: the clock and the transport
    Once("a clock tick", {0xF8});
    Once("Start", {0xFA});
    Once("Continue", {0xFB});
    Once("Stop", {0xFC});
    // #157: PLAY after a pause, the Song Position at the next 16th; it read twice
    Once("Song Position (#157)", {0xF2, 0x0C, 0x00});
    Once("Song Position, the largest", {0xF2, 0x7F, 0x7F});
    // System Common's other two, which FRIZZ doesn't send
    Once("MTC quarter frame", {0xF1, 0x23});
    Once("Song Select", {0xF3, 0x05});
    // MIDI feedback and the controls: channel messages, several in one transfer (NormalPage.h)
    Once("a CC", {0xB4, 0x0E, 0x40});
    Once("three CCs at once", {0xB0, 0x0E, 0x01, 0xB1, 0x0F, 0x02, 0xB2, 0x10, 0x03});
    Once("a note on and off", {0x90, 0x3C, 0x64, 0x80, 0x3C, 0x00});
    Once("a program change", {0xC0, 0x05});
    // the answers: FRIZZ's SysEx, every length round a packet's three bytes
    for (size_t data = 0; data <= 7; data++)
    {
        std::vector<uint8_t> msg = {0xF0, 0x7D, 0x43, 0x48};
        for (size_t i = 0; i < data; i++)
            msg.push_back(uint8_t(i));
        msg.push_back(0xF7);
        Once(("a SysEx of " + std::to_string(msg.size()) + " bytes").c_str(), msg);
    }
    return Finish();
}
