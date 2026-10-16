// uartlisten.cpp: the MIDI jack's listening start, its USART's part (libDaisy's per/uart_listen.h,
// DmaListenStart's in uart.cpp), on registers of its own that note every write. The first start
// sets the character match on 0xF8 (#322), with the USART off for it; a start again after a
// receive error leaves the USART on, as turning UE off stops the transmitter too and cut the
// byte MIDI out was sending (#442). The chip itself: midisuite.py's trs-restart-tx (the bench
// build). Run by unit.sh uartlisten.
// unit includes: ../code/libs/libDaisy/src
#include <cstdint>
#include <vector>
#include "check.h"

// the bits, as stm32h750xx.h has them
#define USART_CR1_UE 0x00000001UL
#define USART_CR1_RE 0x00000004UL
#define USART_CR1_TE 0x00000008UL
#define USART_CR1_CMIE 0x00004000UL
#define USART_CR2_ADDM7 0x00000010UL
#define USART_CR2_STOP 0x00003000UL
#define USART_CR2_ADD_Pos 24U
#define USART_CR2_ADD 0xFF000000UL
#define USART_ICR_CMCF 0x00020000UL
#include "per/uart_listen.h"

struct Write
{
    char reg;
    uint32_t value;
};
static std::vector<Write> writes;

/** A register that notes each write; reads give what was last written */
struct Reg
{
    char name;
    uint32_t value;
    Reg& operator=(uint32_t v)
    {
        value = v;
        writes.push_back({name, v});
        return *this;
    }
    operator uint32_t() const { return value; }
};

struct Usart
{
    Reg CR1{'1', 0}, CR2{'2', 0}, ICR{'I', 0};
};

static const uint32_t kMatch = (0xF8U << USART_CR2_ADD_Pos) | USART_CR2_ADDM7;
static const uint32_t kOn = USART_CR1_UE | USART_CR1_RE | USART_CR1_TE; // HAL_UART_Init's

/** Writes to CR1 with UE clear: each one stops the transmitter */
static int Offs()
{
    int n = 0;
    for (const Write& w : writes)
        n += w.reg == '1' && !(w.value & USART_CR1_UE);
    return n;
}
static int WritesTo(char reg)
{
    int n = 0;
    for (const Write& w : writes)
        n += w.reg == reg;
    return n;
}

static void Start(Usart& u)
{
    writes.clear();
    daisy::UartListenMatchTick(&u);
}

/** Ends as every start must: the match set, its flag cleared, its interrupt and the USART on */
static void Ends(const Usart& u, uint32_t cr2, const char* when)
{
    char what[160];
    snprintf(what, sizeof what, "%s: CR2 has ADD 0xF8 with ADDM7, the rest as it was", when);
    Check(u.CR2 == ((cr2 & ~(USART_CR2_ADD | USART_CR2_ADDM7)) | kMatch), what);
    snprintf(what, sizeof what, "%s: CR1 on, the match's interrupt too", when);
    Check(u.CR1 == (kOn | USART_CR1_CMIE), what);
    snprintf(what, sizeof what, "%s: the match's flag cleared", when);
    Check(WritesTo('I') == 1 && u.ICR == USART_ICR_CMCF, what);
}

int main()
{
    // the first start, after HAL_UART_Init (CR2: STOP 1, no address)
    Usart u;
    u.CR1.value = kOn;
    u.CR2.value = 0;
    Start(u);
    Ends(u, 0, "first start");
    Check(Offs() == 1, "first start: the USART off once, for ADD (written only with UE clear)");
    bool add_while_off = false, off = false;
    for (const Write& w : writes)
    {
        if (w.reg == '1')
            off = !(w.value & USART_CR1_UE);
        if (w.reg == '2')
            add_while_off = off;
    }
    Check(add_while_off, "first start: CR2 written while the USART is off");

    // started again after a receive error: HAL ended the reception (CMIE stays); the USART on
    Start(u);
    Ends(u, 0, "again");
    Check(Offs() == 0, "again: the USART never off, the byte MIDI out sends goes on (#442)");
    Check(WritesTo('2') == 0, "again: CR2 untouched");

    // again after HAL_UART_Init (its UART_SetConfig writes only STOP in CR2), the match's
    // interrupt off
    u.CR2.value = (u.CR2.value & ~USART_CR2_STOP) | 0x2000;
    u.CR1.value = kOn;
    Start(u);
    Ends(u, 0x2000, "after HAL_UART_Init");
    Check(Offs() == 0, "after HAL_UART_Init: CR2 kept ADD, the USART never off");

    // CR2 not as the match needs: another address, or ADD without ADDM7 (4-bit detection)
    const uint32_t odd[] = {(0x0AU << USART_CR2_ADD_Pos) | USART_CR2_ADDM7, 0xF8U << USART_CR2_ADD_Pos};
    for (const uint32_t cr2 : odd)
    {
        u.CR2.value = cr2 | 0x2000;
        u.CR1.value = kOn | USART_CR1_CMIE;
        Start(u);
        Ends(u, 0x2000, cr2 & USART_CR2_ADDM7 ? "another address" : "ADD without ADDM7");
        Check(Offs() == 1, "CR2 without the match: set as at the first start, the USART off once");
    }
    return Finish();
}
