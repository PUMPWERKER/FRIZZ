// Host stand-in: the twin runs the audio callback and main() in turn, never at once. At a
// preempt point a check names (FRIZZ_PREEMPT, twin::PreemptAt) the callback comes in the middle
// of main() instead, as the interrupt may on the device, unless a ScopedIrqBlocker holds it off
#pragma once
namespace twin
{
extern int irq_blocked;
void Preempt(const char* point);
} // namespace twin
namespace daisy
{
class ScopedIrqBlocker
{
public:
    ScopedIrqBlocker() { twin::irq_blocked++; }
    ~ScopedIrqBlocker() { twin::irq_blocked--; }
};
} // namespace daisy
#define FRIZZ_PREEMPT(point) twin::Preempt(point)
