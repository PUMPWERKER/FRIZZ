// rtalloc.cpp: the twin on a real-time thread (twin.h). A host that runs the virtual CHOMPI from
// its audio thread must not allocate there: Run(), Midi(), UsbMidi() and the Take...(buf, max)
// calls don't, with MIDI going in on both ports and out of both (answers over USB, a DAW's clock
// passed on to the jack) and USB audio both ways. operator new and malloc are replaced
// in this process and counted while the twin plays, after a warm-up (the card's saves of what
// it set allocate, twin.h). And the queues' limits: a full one drops the newest, counted in
// Dropped(), and the taking calls that allocate still give everything. Run by unit.sh rtalloc.
#include <cmath>
#include <cstdlib>
#include <new>
#include "check.h"
#include "twin.h"

using namespace twin;

// ======== every allocation in this process, counted while counting is on ========
static bool counting = false;
static long allocations = 0;

#ifdef __GLIBC__
// C's too, for anything that calls malloc itself
extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_calloc(size_t, size_t);
extern "C" void* __libc_realloc(void*, size_t);
static void* Raw(size_t n) { return __libc_malloc(n); }
extern "C" void* malloc(size_t n)
{
    allocations += counting;
    return __libc_malloc(n);
}
extern "C" void* calloc(size_t n, size_t size)
{
    allocations += counting;
    return __libc_calloc(n, size);
}
extern "C" void* realloc(void* p, size_t n)
{
    allocations += counting;
    return __libc_realloc(p, n);
}
#else
static void* Raw(size_t n) { return malloc(n); }
#endif

void* operator new(size_t n, const std::nothrow_t&) noexcept
{
    allocations += counting;
    return Raw(n ? n : 1);
}
void* operator new[](size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void* operator new(size_t n)
{
    void* p = operator new(n, std::nothrow);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// ======== the host: a block at a time, as an audio thread runs it ========
static float in[kBlockSize * kChannels], out[kBlockSize * kChannels], usb_in[kBlockSize * 2];
static float phase = 0.f;
static double tick_next = 0.;
static const double kTickMs = 60000. / (120. * 24.); // a DAW's clock at 120 BPM, over USB

// what the host took, without allocating
static uint8_t usb_taken[64];
static MidiOutByte midi_taken[64];
static float audio_taken[kBlockSize * 2];
struct Taken
{
    long usb_bytes, usb_answers, trs_ticks, frames, calls;
};
static Taken taken;

static void Usb(std::initializer_list<int> bytes)
{
    for (int b : bytes)
        UsbMidi(static_cast<uint8_t>(b));
}

/** One block: a sine into AUX and from the computer, the clock over USB, then what came
 *  out taken a little at a time (a partial take leaves the rest for the next) */
static void Block(bool take = true)
{
    for (size_t i = 0; i < kBlockSize; i++)
    {
        const float s = .2f * sinf(phase);
        phase = fmodf(phase + 2.f * float(M_PI) * 440.f / kSampleRate, 2.f * float(M_PI));
        in[i * kChannels + 2] = in[i * kChannels + 3] = s;
        usb_in[i * 2] = usb_in[i * 2 + 1] = s;
    }
    while (tick_next <= BlockMs())
    {
        UsbMidi(0xF8);
        tick_next += kTickMs;
    }
    SetUsbAudioIn(usb_in);
    Run(1, in, out);
    if (!take)
        return;
    size_t n;
    while ((n = TakeUsbOut(usb_taken, 16)) > 0)
    {
        taken.calls++;
        taken.usb_bytes += n;
        for (size_t i = 0; i < n; i++)
            taken.usb_answers += usb_taken[i] == 0xF0;
    }
    while ((n = TakeMidiOut(midi_taken, 8)) > 0)
    {
        taken.calls++;
        for (size_t i = 0; i < n; i++)
            taken.trs_ticks += !midi_taken[i].usb && midi_taken[i].byte == 0xF8;
    }
    while ((n = TakeUsbAudioOut(audio_taken, kBlockSize / 2)) > 0)
    {
        taken.calls++;
        taken.frames += n;
    }
}

static void Ms(int ms, bool take = true)
{
    for (int b = 0; b < ms * 2; b++)
        Block(take);
}

/** What a host plays at the twin for ms: the clock (Block's), notes on channel 16 into the
 *  jack and over USB, the knobs' values asked over USB (MidiControl.h's kCmdParams, a SysEx
 *  answered over USB) */
static void Play(int ms)
{
    for (int t = 0; t < ms; t += 50)
    {
        Usb({0x9F, 60, 100});
        for (uint8_t b : {0x9F, 62, 100})
            Midi(b);
        Ms(25);
        Usb({0x8F, 60, 0});
        for (uint8_t b : {0x8F, 62, 0})
            Midi(b);
        Usb({0xF0, 0x7D, 0x43, 0x48, 0x21, 4, 0xF7});
        Ms(25);
    }
}

static bool NoneDropped()
{
    const Drops d = Dropped();
    return d.midi_in == 0 && d.usb_midi_in == 0 && d.usb_out == 0 && d.midi_out == 0 &&
           d.usb_audio_out == 0;
}

int main()
{
    Boot();
    Ms(6000);
    Check(MainLoopRunning(), "rtalloc: the firmware runs");

    // warm-up: MIDI out on, to the jack (its setting saved to the card 2 s later), the
    // computer recording, then the same play as below once, none of it counted
    Usb({0xF0, 0x7D, 0x43, 0x48, 0x13, 0x03, 1, 0xF7});
    SetUsbRecording(true);
    Ms(3000);
    Play(1000);

    taken = {};
    counting = true;
    Play(3000);
    counting = false;
    char what[160];
    snprintf(what, sizeof(what),
             "rtalloc: 3 s of MIDI in and out and USB audio, no allocation in Run(), Midi(), "
             "UsbMidi() or the Take...(buf, max) calls (%ld)",
             allocations);
    Check(allocations == 0, what);
    Check(taken.usb_answers >= 50 && taken.usb_bytes > 0, "rtalloc: and the answers came over USB, taken");
    Check(taken.trs_ticks > 100, "rtalloc: the DAW's clock went on out of the jack, taken");
    Check(taken.frames == 3000 * 2 * long(kBlockSize), "rtalloc: every frame the computer recorded, taken");
    Check(taken.calls > taken.frames / long(kBlockSize), "rtalloc: in pieces: what a take leaves stays for the next");
    Check(NoneDropped(), "rtalloc: nothing dropped");

    // the calls that return a string or vector give the same
    Usb({0xF0, 0x7D, 0x43, 0x48, 0x21, 4, 0xF7});
    Ms(20, false);
    const std::string answer = TakeUsbOut();
    Check(answer.size() == 23 && static_cast<uint8_t>(answer[0]) == 0xF0 && static_cast<uint8_t>(answer[4]) == 0x61,
          "rtalloc: TakeUsbOut() still gives the whole answer");
    const std::vector<MidiOutByte> sent = TakeMidiOut();
    const std::vector<float> recorded = TakeUsbAudioOut();
    Check(!sent.empty() && recorded.size() == 20 * 2 * kBlockSize * 2,
          "rtalloc: TakeMidiOut() and TakeUsbAudioOut() still give everything");

    // full: the computer records more than kUsbAudioOutFrames without anything taken; a block
    // that doesn't fit is dropped whole, the rest kept
    Ms(1000 + 5, false);
    Check(Dropped().usb_audio_out == 10 * kBlockSize && TakeUsbAudioOut().size() == kUsbAudioOutFrames * 2,
          "rtalloc: USB audio out full: the newest blocks dropped, whole, and counted");
    TakeUsbOut();
    TakeMidiOut();
    // MIDI in full: more than kMidiInCapacity bytes for one block; the newest are dropped
    for (size_t i = 0; i < kMidiInCapacity + 5; i++)
        Midi(0xFE);
    Check(Dropped().midi_in == 5 && Dropped().usb_midi_in == 0, "rtalloc: MIDI in full: the newest bytes dropped, counted");
    Ms(100);
    Check(MainLoopRunning() && Dropped().midi_in == 5, "rtalloc: and the firmware runs on");
    return Finish();
}
