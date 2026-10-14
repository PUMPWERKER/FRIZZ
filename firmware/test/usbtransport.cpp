// usbtransport.cpp: FRIZZ's USB MIDI transport (code/src/UsbMidiTransport.h, TinyUSB's
// MIDI class behind libDaisy's MidiHandler) against a host in memory (usbtransport/tusb.h).
// MidiHandler::Listen() must start the receive (RxActive false until StartRx: lost-tty's port
// said true from the start and was deaf to USB MIDI, so the PoC's first build on the device
// answered nothing, #196); what the host sends reaches the parser, FRIZZ's answers reach the host whole,
// a queue too full refuses a message whole, Reset() and an unmounted host drop the queue, and so
// does a host that doesn't read for kStalePasses, keeping a message TinyUSB has begun. The
// virtual CHOMPI can't show it: its USB is its own (twin/host/hid/usb_midi.h). Run by unit.sh
// usbtransport.
// unit includes: usbtransport ../code/libs/libDaisy/src ../code/libs/libDaisy/src/sys
#include "../code/libs/libDaisy/src/hid/midi_parser.cpp"
#include "hid/midi.h"
#include "UsbMidiTransport.h"
#include "check.h"
#include <string>
#include <vector>

namespace tusbstub
{
std::vector<uint8_t> from_host, to_host;
bool mounted = true;
uint32_t room = 64;
} // namespace tusbstub

uint32_t tud_midi_available() { return static_cast<uint32_t>(tusbstub::from_host.size()); }
uint32_t tud_midi_stream_read(void* buffer, uint32_t bufsize)
{
    uint32_t n = std::min<uint32_t>(bufsize, static_cast<uint32_t>(tusbstub::from_host.size()));
    std::copy(tusbstub::from_host.begin(), tusbstub::from_host.begin() + n, static_cast<uint8_t*>(buffer));
    tusbstub::from_host.erase(tusbstub::from_host.begin(), tusbstub::from_host.begin() + n);
    return n;
}
bool tud_midi_mounted() { return tusbstub::mounted; }
uint32_t tud_midi_stream_write(uint8_t, const uint8_t* buffer, uint32_t bufsize)
{
    uint32_t n = std::min(bufsize, tusbstub::room);
    tusbstub::to_host.insert(tusbstub::to_host.end(), buffer, buffer + n);
    return n;
}

namespace
{
using Handler = daisy::MidiHandler<chompi::MidiTinyUsbTransport>;

std::string Events(Handler& h)
{
    std::string s;
    while (h.HasEvents())
    {
        daisy::MidiEvent e = h.PopEvent();
        char b[64];
        if (e.type == daisy::NoteOn)
            snprintf(b, sizeof b, "on %02x %02x", e.data[0], e.data[1]);
        else if (e.type == daisy::SystemRealTime)
            snprintf(b, sizeof b, "rt");
        else if (e.type == daisy::SystemCommon && e.sc_type == daisy::SystemExclusive)
        {
            std::string d;
            for (size_t i = 0; i < e.sysex_message_len; i++)
            {
                char x[4];
                snprintf(x, sizeof x, "%s%02x", d.empty() ? "" : " ", e.sysex_data[i]);
                d += x;
            }
            snprintf(b, sizeof b, "sysex %s", d.c_str());
        }
        else
            snprintf(b, sizeof b, "type %d", int(e.type));
        s += (s.empty() ? "" : ", ") + std::string(b);
    }
    return s;
}

void Same(const std::string& got, const std::string& want, const char* what)
{
    Check(got == want, what);
    if (got != want)
        printf("      got \"%s\", want \"%s\"\n", got.c_str(), want.c_str());
}
} // namespace

int main()
{
    Handler h;
    Handler::Config config;
    h.Init(config);
    Check(!h.GetMutableTransport().RxActive(), "not listening before Listen()");
    h.Listen(); // as MidiClock::Init does
    Check(h.GetMutableTransport().RxActive(), "Listen() starts the receive (deaf otherwise, #196)");
    Check(!chompi::MidiTinyUsbTransport::Heard(), "nothing heard yet");

    // in: an IDENT query as the tools send it, a note, a tick
    tusbstub::from_host = {0xF0, 0x7D, 0x43, 0x48, 0x3E, 0x00, 0x01, 0xF7, 0x90, 0x3C, 0x64, 0xF8};
    chompi::MidiTinyUsbTransport::Process();
    Same(Events(h), "sysex 7d 43 48 3e 00 01, on 3c 64, rt", "the host's SysEx, a note and a tick reach the parser");
    Check(chompi::MidiTinyUsbTransport::Heard(), "heard once something was parsed (UsbDevice.cpp's Watch)");
    // more than one read's 64 bytes at once
    tusbstub::from_host.clear();
    for (int i = 0; i < 30; i++)
        tusbstub::from_host.insert(tusbstub::from_host.end(), {0x90, uint8_t(i), 0x40});
    chompi::MidiTinyUsbTransport::Process();
    size_t notes = 0;
    while (h.HasEvents())
        notes += h.PopEvent().type == daisy::NoteOn;
    Check(notes == 30, "90 bytes in one pass: every note");

    // out: an answer of 45 bytes, the most MidiControl::Reply sends
    chompi::MidiTinyUsbTransport& t = h.GetMutableTransport();
    std::vector<uint8_t> answer = {0xF0, 0x7D, 0x43, 0x48, 0x7E};
    while (answer.size() < 44)
        answer.push_back(uint8_t(answer.size()));
    answer.push_back(0xF7);
    Check(t.Tx(answer.data(), answer.size()), "an answer queued");
    chompi::MidiTinyUsbTransport::Process();
    Check(tusbstub::to_host == answer, "an answer reaches the host whole");

    // the host's FIFO takes 16 bytes a pass: the rest goes at the next
    tusbstub::to_host.clear();
    tusbstub::room = 16;
    t.Tx(answer.data(), answer.size());
    for (int pass = 0; pass < 4; pass++)
        chompi::MidiTinyUsbTransport::Process();
    Check(tusbstub::to_host == answer, "a slow host gets it whole, over several passes");
    tusbstub::room = 64;

    // full: refused whole, nothing torn
    tusbstub::to_host.clear();
    size_t queued = 0;
    while (t.Tx(answer.data(), answer.size()))
        queued++;
    Check(queued == chompi::MidiTinyUsbTransport::kTxSize / answer.size(), "the queue takes whole answers");
    Check(t.Room() < answer.size(), "the one that didn't fit is refused whole");
    for (int pass = 0; pass < 10; pass++)
        chompi::MidiTinyUsbTransport::Process();
    Check(tusbstub::to_host.size() == queued * answer.size(), "every queued answer goes");

    // Reset(), or a host gone: the queue dropped, not sent later
    tusbstub::to_host.clear();
    t.Tx(answer.data(), answer.size());
    t.Reset();
    chompi::MidiTinyUsbTransport::Process();
    Check(tusbstub::to_host.empty() && t.Room() == chompi::MidiTinyUsbTransport::kTxSize, "Reset() drops the queue");
    tusbstub::mounted = false;
    t.Tx(answer.data(), answer.size());
    chompi::MidiTinyUsbTransport::Process();
    tusbstub::mounted = true;
    chompi::MidiTinyUsbTransport::Process();
    Check(tusbstub::to_host.empty(), "without a host the queue is dropped");

    // a host that has FRIZZ but doesn't read (Linux, no app on the port): the clock fills the
    // queue, which is dropped after kStalePasses, so an app opening the port later gets no
    // backlog (on main ~270 ticks in a burst, measured on the device), and SendUsb doesn't wait
    using T = chompi::MidiTinyUsbTransport;
    tusbstub::room = 0;
    uint8_t tick = 0xF8;
    while (t.Tx(&tick, 1))
        ;
    Check(!t.Stuck(), "not stuck at once");
    for (uint32_t pass = 0; pass < T::kStuckPasses; pass++)
        T::Process();
    Check(t.Stuck(), "stuck after kStuckPasses: SendUsb doesn't wait for room");
    Check(t.Room() == 0, "kept until kStalePasses");
    for (uint32_t pass = T::kStuckPasses; pass < T::kStalePasses; pass++)
        T::Process();
    Check(t.Room() == T::kTxSize, "dropped after kStalePasses: a reply fits again");
    tusbstub::room = 64;
    t.Tx(&tick, 1);
    T::Process();
    Same(std::to_string(tusbstub::to_host.size()), "1", "an app opening the port gets the new tick, no backlog");
    Check(!t.Stuck(), "read again: not stuck");

    // the host stops reading in the middle of an answer: TinyUSB has begun a SysEx and would
    // pack whatever follows into it, so the drop keeps the answer through its F7
    tusbstub::to_host.clear();
    tusbstub::room = 10;
    t.Tx(answer.data(), answer.size());
    T::Process();
    tusbstub::room = 0;
    for (int i = 0; i < 3; i++)
        t.Tx(&tick, 1);
    for (uint32_t pass = 0; pass < T::kStalePasses; pass++)
        T::Process();
    tusbstub::room = 64;
    t.Tx(&tick, 1);
    T::Process();
    std::vector<uint8_t> want = answer;
    want.push_back(0xF8);
    Check(tusbstub::to_host == want, "a SysEx cut by a stall ends with its F7, then the new tick");

    // the same within a CC: its data bytes kept, the ticks after it dropped
    tusbstub::to_host.clear();
    uint8_t cc[3] = {0xB0, 0x10, 0x40};
    tusbstub::room = 1;
    t.Tx(cc, 3);
    T::Process();
    tusbstub::room = 0;
    for (int i = 0; i < 3; i++)
        t.Tx(&tick, 1);
    for (uint32_t pass = 0; pass < T::kStalePasses; pass++)
        T::Process();
    tusbstub::room = 64;
    t.Tx(&tick, 1);
    T::Process();
    Check(tusbstub::to_host == std::vector<uint8_t>({0xB0, 0x10, 0x40, 0xF8}), "a CC cut by a stall whole, then the new tick");
    return Finish();
}
