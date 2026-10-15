/* chromium-midi-shim.c: LD_PRELOADed into the headless Chromium that companion.py drives (#399),
 * never into anything else.
 *
 * Chromium's ALSA MIDI (media/midi/midi_manager_alsa.cc) publishes ports only while the sound
 * cards' MIDI devices it counts equal the sequencer's kernel clients from 16 on. It counts each
 * card's rawmidi devices, plus every hwdep whose interface is OPL2, OPL3 or OPL4, as those
 * synths make a sequencer client of their own. The Scarlett's "Focusrite Control" hwdep reports
 * interface 0, which is OPL2. Chromium then counts the Scarlett as two MIDI devices, the counts
 * never meet, and Web MIDI shows only "Midi Through", for every card, FRIZZ's too.
 *
 * snd_ctl_hwdep_info() is wrapped here: an OPL interface on a hwdep whose id doesn't say OPL is
 * reported as another (SND_HWDEP_IFACE_LAST + 1). Nothing else changes.
 *
 * Built by companion.py into ~/.cache/frizz-companion (cc -shared -fPIC). No alsa-lib headers:
 * snd_hwdep_info_t's layout is the kernel's struct snd_hwdep_info (include/uapi/sound/asound.h):
 * device, card, id[64], name[80], iface. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>

struct hwdep_info
{
    unsigned int device;
    int card;
    unsigned char id[64];
    unsigned char name[80];
    int iface;
    unsigned char reserved[64];
};

enum { IFACE_OPL2 = 0, IFACE_OPL3 = 1, IFACE_OPL4 = 2, IFACE_OTHER = 64 };

int snd_ctl_hwdep_info(void* ctl, void* info)
{
    static int (*real)(void*, void*);
    if (!real)
        real = (int (*)(void*, void*))dlsym(RTLD_NEXT, "snd_ctl_hwdep_info");
    if (!real)
        return -1;
    const int err = real(ctl, info);
    if (err == 0)
    {
        struct hwdep_info* h = (struct hwdep_info*)info;
        const int opl = h->iface == IFACE_OPL2 || h->iface == IFACE_OPL3 || h->iface == IFACE_OPL4;
        if (opl && !strstr((const char*)h->id, "OPL") && !strstr((const char*)h->name, "OPL"))
            h->iface = IFACE_OTHER;
    }
    return err;
}
