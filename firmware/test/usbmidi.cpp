// usbmidi.cpp: libDaisy's USB MIDI out (platform/daisy/libs/libDaisy/src/hid/usb_midi.cpp, compiled here
// as it is) against what a host makes of it. libDaisy packs each message into USB MIDI packets
// of 4 bytes, a code index number (CIN) first; the host's driver (ALSA's snd-usb-audio, and the
// USB MIDI 1.0 spec) takes as many bytes from each as its CIN says. Every message FRIZZ sends
// over USB (MidiOut.h's clock and transport, MIDI feedback's CCs, the answers' SysEx) must
// arrive once and whole. The virtual CHOMPI can't show this: its USB passes raw bytes, the
// packets left out (twin/host/hid/usb_midi.h). The libdaisy.a FRIZZ links is built from this
// source (THIRD_PARTY.md). And what comes in: a host's packets through usb_midi.cpp and libDaisy's
// MIDI parser (midi_parser.cpp, also as it is), as the events FRIZZ reads (MidiClock.h), the
// jack's bytes through the parser alone. Run by unit.sh usbmidi.
// unit includes: usbmidi ../platform/daisy/libs/libDaisy/src ../platform/daisy/libs/libDaisy/src/sys
#include "../platform/daisy/libs/libDaisy/src/hid/usb_midi.cpp"
#include "../platform/daisy/libs/libDaisy/src/hid/midi_parser.cpp"
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

// ---- in ----
MidiParser parser;
std::string heard;

void Hear(uint8_t byte)
{
    MidiEvent e;
    if (!parser.Parse(byte, &e))
        return;
    char b[64];
    if (e.type == NoteOn || e.type == NoteOff)
        snprintf(b, sizeof b, "%s %02x %02x", e.type == NoteOn ? "on" : "off", e.data[0], e.data[1]);
    else if (e.type == ControlChange)
        snprintf(b, sizeof b, "cc %02x %02x", e.data[0], e.data[1]);
    else if (e.type == SystemRealTime)
        snprintf(b, sizeof b, "rt %02x", 0xF8 | e.srt_type);
    else if (e.type == SystemCommon && e.sc_type == SystemExclusive)
        snprintf(b, sizeof b, "sysex %s", Hex(std::vector<uint8_t>(e.sysex_data, e.sysex_data + e.sysex_message_len)).c_str());
    else
        snprintf(b, sizeof b, "type %d", e.type);
    heard += (heard.empty() ? "" : ", ") + std::string(b);
}

void Received(uint8_t* data, size_t size, void*)
{
    for (size_t i = 0; i < size; i++)
        Hear(data[i]);
}

/** packets from a host, one transfer: FRIZZ hears what it expects */
void FromHost(const char* what, std::vector<uint8_t> packets, const char* expect)
{
    parser.Reset();
    heard.clear();
    uint32_t n = static_cast<uint32_t>(packets.size());
    ReceiveCallback(packets.data(), &n);
    const std::string text = std::string(what) + ": hears \"" + heard + "\"";
    Check(heard == expect, text.c_str());
}

/** bytes into the jack: FRIZZ hears what it expects */
void FromJack(const char* what, std::vector<uint8_t> bytes, const char* expect)
{
    parser.Reset();
    heard.clear();
    for (uint8_t b : bytes)
        Hear(b);
    const std::string text = std::string(what) + ": hears \"" + heard + "\"";
    Check(heard == expect, text.c_str());
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

    // in: a SysEx cut short (its sender unplugged) ends at the next status byte, which starts
    // a message of its own (#166); a real-time byte inside it is heard and it carries on (#18)
    usb.StartRx(Received, nullptr);
    FromJack("the jack: a SysEx cut by a note (#166)", {0xF0, 0x7D, 0x43, 0x90, 0x3C, 0x64, 0x80, 0x3C, 0x00},
             "on 3c 64, off 3c 00");
    FromJack("the jack: a SysEx cut by a CC in running status", {0xB0, 0x0E, 0x01, 0xF0, 0x7D, 0xB0, 0x0E, 0x02, 0x0F, 0x03},
             "cc 0e 01, cc 0e 02, cc 0f 03");
    FromJack("the jack: a SysEx cut by another", {0xF0, 0x01, 0x02, 0xF0, 0x7D, 0x43, 0x48, 0x10, 0xF7},
             "sysex 7d 43 48 10");
    FromJack("the jack: a tick inside a SysEx (#18)", {0xF0, 0x7D, 0xF8, 0x43, 0xF7}, "rt f8, sysex 7d 43");
    FromHost("USB: a SysEx cut by a note (#166)",
             {0x04, 0xF0, 0x7D, 0x43, 0x09, 0x90, 0x3C, 0x64, 0x08, 0x80, 0x3C, 0x00}, "on 3c 64, off 3c 00");
    FromHost("USB: a tick inside a SysEx (#18)",
             {0x04, 0xF0, 0x7D, 0x43, 0x0F, 0xF8, 0x00, 0x00, 0x06, 0x48, 0xF7, 0x00}, "rt f8, sysex 7d 43 48");
    return Finish();
}
