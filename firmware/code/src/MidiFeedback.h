/** @file MidiFeedback.h
 *  @brief MIDI feedback (#104, MANUAL.md "MIDI feedback"): what the hand changes on the panel
 *  goes out as the CCs that would set it (MidiControl.h's map), so a controller's LED rings
 *  or motor faders, or a DAW's takeover, follow the panel. Off at first (the settings page's
 *  upper D#); it goes where MIDI out goes (MidiOut.h).
 *
 *  It compares rather than queues: a table holds the 7-bit value each CC was last sent with
 *  (or heard with: what came in by MIDI or SysEx is never sent back, NormalPage::Remote), and
 *  a sweep every kSweepMs sends what differs from it, from MainLoop. So a value goes out at
 *  most once a sweep and only when its 7 bits change, a scene's many changes go once, and a
 *  sweep that can't send (USB busy, the jack's ring full) goes on from there at the next pass:
 *  nothing is lost, only late. While a morph runs the effects' values aren't swept, so only
 *  where it lands goes out.
 *
 *  The jack's bytes wait in a ring of their own (FeedbackRing), which MidiOut sends from the
 *  audio callback after the clock's, a byte a block (about 660 CCs a second): a tick still
 *  goes at once, also between a message's bytes, as MIDI lets real-time bytes.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace chompi
{

/** A byte ring between the audio callback and MainLoop, one side writing, the other reading:
 *  whole messages go in at once, so the reader never sees half of one. MIDI out's
 *  (MidiOut.h) and the jack's feedback bytes */
template <uint32_t kSize>
class ByteRing
{
public:
    /** All of n bytes, or none: false if they don't fit */
    bool Push(const uint8_t* msg, size_t n)
    {
        uint32_t head = head_;
        if (kSize - (head - tail_) < n)
            return false;
        for (size_t i = 0; i < n; i++)
            buf_[head++ % kSize] = msg[i];
        head_ = head;
        return true;
    }
    inline size_t Room() const { return kSize - (head_ - tail_); }
    inline bool Empty() const { return head_ == tail_; }
    inline uint8_t Front() const { return buf_[tail_ % kSize]; }
    /** The byte i after the front */
    inline uint8_t At(size_t i) const { return buf_[(tail_ + i) % kSize]; }
    inline void Pop() { tail_ = tail_ + 1; }
    inline void Drop(size_t n) { tail_ = tail_ + n; }
    inline void Clear() { tail_ = head_; }

private:
    uint8_t buf_[kSize];
    volatile uint32_t head_ = 0, tail_ = 0;
};

/** The jack's feedback bytes: MainLoop pushes whole messages, the audio callback sends them.
 *  96: 32 CCs, 48 ms of the jack at a byte a block */
typedef ByteRing<96> FeedbackRing;

namespace feedback
{
static const uint8_t kKeys = 15;    // the white keys: their channels 1-15 (MidiControl.h)
static const uint8_t kKeySlots = 9; // a key's knobs 1-8 (CC 14-21), then its latch (CC 22)
static const uint8_t kLatchSlot = 8;
static const uint32_t kSweepMs = 10;
/** What's sent, each its own CC: white key k's slot s on its channel at k * kKeySlots + s,
 *  then the global channel's */
enum Item : uint8_t
{
    kGlobalLatches = kKeys * kKeySlots, // + key 0-13: CC 102-115
    kFocusItems = kGlobalLatches + kKeys - 1, // + slot: CC 14-22, the effect in focus
    kCompItems = kFocusItems + kKeySlots,     // + param: CC 24-31
    kFocus = kCompItems + kKeySlots - 1,      // CC 23
    kOutGain,
    kInGain,
    kMix,
    kHpCue,
    kSpeed,
    kFader,
    kSource,
    kDry,  // CC 116, the source's effects (#177)
    kLive, // CC 117, the source live
    kNumItems,
};
static const uint8_t kNone = 0xFF; // in the table: not sent yet

/** kFocus .. kLive's CCs, in their order */
static constexpr uint8_t kGlobalCc[] = {23, 85, 86, 87, 89, 90, 80, 84, 116, 117};
static_assert(sizeof(kGlobalCc) == kNumItems - kFocus, "a CC per global item");

/** An item's CC number. MidiControl.h checks them against its map (midimap) */
constexpr uint8_t Cc(uint8_t item)
{
    return item < kGlobalLatches ? static_cast<uint8_t>(14 + item % kKeySlots)
         : item < kFocusItems    ? static_cast<uint8_t>(102 + item - kGlobalLatches)
         : item < kCompItems     ? static_cast<uint8_t>(14 + item - kFocusItems)
         : item < kFocus         ? static_cast<uint8_t>(24 + item - kCompItems)
                                 : kGlobalCc[item - kFocus];
}

/** A 0-1 knob as 7 bits, the inverse of MidiToKnob (MidiControl.h): .5 is 64, 1 is 127 */
inline uint8_t KnobTo7(float k)
{
    k = k < 0.f ? 0.f : (k > 1.f ? 1.f : k);
    return static_cast<uint8_t>(k <= .5f ? k * 128.f + .5f : 64.f + (k - .5f) * 126.f + .5f);
}
/** The loop's speed in semitones as CC 90 has it: -48 is 0, 0 is 64, 12 is 127 */
inline uint8_t SemisTo7(int semis)
{
    return static_cast<uint8_t>(semis <= 0 ? (semis + 48) * 64 / 48 : 64 + (semis * 63 + 6) / 12);
}
} // namespace feedback

class MidiFeedback
{
public:
    /** Every item as it stands, so nothing goes out until it changes: feedback switched on.
     *  value(item, v): its value now, false for none to send */
    template <class Value>
    void Reset(Value value)
    {
        for (uint8_t i = 0; i < feedback::kNumItems; i++)
        {
            uint8_t v;
            shadow_[i] = value(i, v) ? v : feedback::kNone;
        }
        cursor_ = feedback::kNumItems;
    }

    /** item as it stands now is what the other side already has: it came in by MIDI or
     *  SysEx, so it isn't sent back */
    template <class Value>
    void Heard(uint8_t item, Value value)
    {
        uint8_t v;
        if (item < feedback::kNumItems && value(item, v))
            shadow_[item] = v;
    }
    /** The same with the value it'll have (the looper's speed, a block from now) */
    inline void Heard(uint8_t item, uint8_t v) { shadow_[item] = v; }

    /** One pass of the sweep, from MainLoop: up to kPerPass changed values as CCs to
     *  send(msgs, n), which says whether they went; a new sweep kSweepMs after the last
     *  began. global: the global channel 1-16 */
    template <class Value, class Send>
    void Pass(uint32_t now, uint8_t global, Value value, Send send)
    {
        using namespace feedback;
        if (cursor_ >= kNumItems)
        {
            if (now - swept_at_ < kSweepMs)
                return;
            cursor_ = 0;
            swept_at_ = now;
        }
        uint8_t msg[kPerPass * 3], item[kPerPass], val[kPerPass];
        size_t n = 0;
        uint8_t c = cursor_;
        for (; c < kNumItems && n < kPerPass; c++)
        {
            uint8_t v;
            if (!value(c, v) || v == shadow_[c])
                continue;
            msg[3 * n] = static_cast<uint8_t>(0xB0 | (c < kGlobalLatches ? c / kKeySlots : global - 1));
            msg[3 * n + 1] = Cc(c);
            msg[3 * n + 2] = v;
            item[n] = c;
            val[n++] = v;
        }
        if (n && !send(msg, n))
            return; // again from cursor_ at the next pass
        for (size_t i = 0; i < n; i++)
            shadow_[item[i]] = val[i];
        cursor_ = c;
    }

    static const size_t kPerPass = 16; // 48 bytes: one USB transfer

private:
    uint8_t shadow_[feedback::kNumItems];
    uint8_t cursor_ = feedback::kNumItems; // the sweep's next item; kNumItems between sweeps
    uint32_t swept_at_ = 0;
};

} // namespace chompi
