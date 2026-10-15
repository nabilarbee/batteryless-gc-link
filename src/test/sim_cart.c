// Simulated bootleg batteryless cart (see sim_cart.h).
//
// The chip models follow the AMD (JEDEC) and Intel command sets closely
// enough to catch wrong sequences, wrong addresses, missing resets,
// programming without an erase and status polling mistakes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_cart.h"

// Hooks used by repro_hw.h in REPRO_HOST_TEST builds
uint16_t HostRomRead16(uint32_t offset);
void HostRomWrite16(uint32_t offset, uint16_t value);
uint8_t HostSramRead8(uint32_t offset);
void HostSramWrite8(uint32_t offset, uint8_t value);
void HostBankWrite8(uint8_t value);

static enum SimBoard sBoard;
static uint8_t *sRom;
static uint8_t sSram[SIM_SRAM_SIZE];
static int sBank;
static struct SimStats sStats;
static uint32_t sEraseBlock;
static char sViolation[256];
static int sFailErase;
static int sFailProgram;
static uint32_t sCfiBlock;       // bus block size the CFI table reports (0: the real one)
static int64_t sEraseInProgress; // block being erased while the chip is busy, or -1
static uint32_t sRandom = 1;

static void Violation(const char *msg, uint32_t offset, uint16_t value)
{
    sStats.violations++;
    snprintf(sViolation, sizeof(sViolation), "%s (offset 0x%X value 0x%04X)", msg, offset, value);
}

const char *SimLastViolation(void) { return sViolation; }
uint8_t *SimRom(void) { return sRom; }
uint8_t *SimSram(void) { return sSram; }
struct SimStats *SimStats(void) { return &sStats; }
void SimSetEraseBlock(uint32_t size) { sEraseBlock = size; }
void SimFailNextErase(void) { sFailErase = 1; }
void SimFailNextProgram(void) { sFailProgram = 1; }
void SimSetCfiBlock(uint32_t size) { sCfiBlock = size; }

static uint8_t Random8(void)
{
    sRandom = sRandom * 1103515245 + 12345;
    return sRandom >> 16;
}

// One item of a CFI table for a chip of 'chipSize' bytes whose blocks are
// 'chipBlock' bytes, optionally with four smaller parameter blocks at the top.
static uint8_t CfiItem(uint32_t index, int intel, uint32_t chipSize, uint32_t chipBlock, int paramBlocks)
{
    uint32_t count, size, log2 = 0;

    switch (index)
    {
    case 0x10: return 'Q';
    case 0x11: return 'R';
    case 0x12: return 'Y';
    case 0x13: return intel ? 0x01 : 0x02;
    case 0x27:
        while ((1u << log2) < chipSize)
            log2++;
        return log2;
    case 0x2C: return paramBlocks ? 2 : 1;
    }
    if (index < 0x2D || index >= 0x2D + (paramBlocks ? 8 : 4))
        return 0;
    if (index < 0x31)
    {
        count = chipSize / chipBlock - (paramBlocks ? 1 : 0);
        size = chipBlock;
    }
    else
    {
        count = 4;
        size = chipBlock / 4;
    }
    switch ((index - 0x2D) % 4)
    {
    case 0: return (count - 1) & 0xFF;
    case 1: return (count - 1) >> 8;
    case 2: return (size / 256) & 0xFF;
    default: return (size / 256) >> 8;
    }
}

static uint32_t ReportedBlock(void)
{
    return sCfiBlock ? sCfiBlock : sEraseBlock;
}

static uint16_t Swap01(uint16_t v)
{
    return (v & ~3) | ((v & 1) << 1) | ((v >> 1) & 1);
}

static uint16_t RomWord(uint32_t offset)
{
    return sRom[offset] | (sRom[offset + 1] << 8);
}

// Clears bits only, like real flash. Returns 0 when the word was not erased.
static int ProgramWord(uint32_t offset, uint16_t value)
{
    uint16_t old = RomWord(offset);
    uint16_t now = old & value;

    sRom[offset] = now;
    sRom[offset + 1] = now >> 8;
    return old == 0xFFFF;
}

static uint32_t sLastErase;

static void EraseBlockAt(uint32_t offset)
{
    uint32_t start = offset & ~(sEraseBlock - 1);
    memset(sRom + start, 0xFF, sEraseBlock);
    sStats.erases++;
    sLastErase = start;
}

uint32_t SimLastErase(void) { return sLastErase; }
uint32_t SimEraseBlock(void) { return sEraseBlock; }

// ------------------------------------------------------------------ AMD

enum { A_READ, A_U1, A_U2, A_AUTOSEL, A_CFI, A_PROG, A_ES, A_EU1, A_EU2, A_BYPASS, A_BYPROG, A_BYEXIT, A_BUSY };

struct AmdLane
{
    int state;
    int afterBusy;       // state to return to when busy ends
    int busyReads;
    uint8_t expect;      // final data of the operation, for DQ7
    int toggle;
    int failed;          // DQ5 set until reset
    int swapped;         // D0/D1 swapped on this lane
    int shift;           // 0 = low byte, 8 = high byte
    int x8;              // byte mode addressing
    uint8_t manufacturer;
    uint16_t device;     // chip-side ID (x16) or per-lane byte (x8)
};

static struct AmdLane sLanes[2];
static int sNumLanes;
static int sAnswersCfiX16; // SIM_AMD_X16 quirk

static uint32_t Unlock1(const struct AmdLane *l) { return l->x8 ? 0xAAA : 0x555; }
static uint32_t Unlock2(const struct AmdLane *l) { return l->x8 ? 0x555 : 0x2AA; }
static uint32_t CfiAddr(const struct AmdLane *l) { return l->x8 ? 0xAA : 0x55; }

// Command byte as the chip sees it on this lane.
static uint8_t LaneCmd(const struct AmdLane *l, uint16_t value)
{
    uint16_t v = (value >> l->shift) & 0xFF;
    return l->swapped ? (uint8_t)Swap01(v) : (uint8_t)v;
}

static uint8_t LaneOut(const struct AmdLane *l, uint8_t chipValue)
{
    return l->swapped ? (uint8_t)Swap01(chipValue) : chipValue;
}

static void AmdStartBusy(struct AmdLane *l, uint8_t expect, int reads, int returnState)
{
    l->state = A_BUSY;
    l->busyReads = reads;
    l->expect = expect;
    l->afterBusy = returnState;
}

static void AmdWrite(int lane, uint32_t gbaOffset, uint16_t value, int *programmed, int *erased)
{
    struct AmdLane *l = &sLanes[lane];
    uint32_t addr = gbaOffset >> 1;      // chip address seen by this lane
    uint8_t cmd = LaneCmd(l, value);

    if (l->state == A_BUSY)
    {
        // After a timeout (DQ5) only a reset is accepted.
        if (l->failed && cmd == 0xF0)
        {
            l->state = A_READ;
            l->failed = 0;
            return;
        }
        Violation("AMD: write while busy", gbaOffset, value);
        return;
    }

    if (cmd == 0xF0 && l->state != A_PROG && l->state != A_BYPROG && l->state != A_BYPASS && l->state != A_BYEXIT)
    {
        l->state = A_READ;
        l->failed = 0;
        return;
    }

    switch (l->state)
    {
    case A_READ:
        if (addr == Unlock1(l) && cmd == 0xAA)
            l->state = A_U1;
        else if (addr == CfiAddr(l) && cmd == 0x98)
            l->state = A_CFI;
        else if (sAnswersCfiX16 && gbaOffset == 0xAA && cmd == 0x98)
            l->state = A_CFI;
        else
            sStats.strayWrites++;
        break;
    case A_U1:
        l->state = (addr == Unlock2(l) && cmd == 0x55) ? A_U2 : A_READ;
        break;
    case A_U2:
        if (addr != Unlock1(l))
            l->state = A_READ;
        else if (cmd == 0x90)
            l->state = A_AUTOSEL;
        else if (cmd == 0xA0)
            l->state = A_PROG;
        else if (cmd == 0x80)
            l->state = A_ES;
        else if (cmd == 0x20)
            l->state = A_BYPASS;
        else
            l->state = A_READ;
        break;
    case A_AUTOSEL:
    case A_CFI:
        break; // only F0 leaves, handled above
    case A_PROG:
    case A_BYPROG:
    {
        int returnState = (l->state == A_BYPROG) ? A_BYPASS : A_READ;
        uint8_t data = (value >> l->shift) & 0xFF;   // GBA-side data
        uint8_t old = sRom[gbaOffset + (l->shift ? 1 : 0)];

        if (old != 0xFF && data != 0xFF)
            Violation("AMD: programming a byte that is not erased", gbaOffset, value);
        sRom[gbaOffset + (l->shift ? 1 : 0)] = old & data;
        *programmed = 1;
        if (sFailProgram)
        {
            l->failed = 1;
            AmdStartBusy(l, data, 1 << 30, returnState);
        }
        else
        {
            AmdStartBusy(l, data, 2 + lane, returnState);
        }
        break;
    }
    case A_ES:
        l->state = (addr == Unlock1(l) && cmd == 0xAA) ? A_EU1 : A_READ;
        break;
    case A_EU1:
        l->state = (addr == Unlock2(l) && cmd == 0x55) ? A_EU2 : A_READ;
        break;
    case A_EU2:
        if (cmd == 0x30)
        {
            *erased = 1;
            if (sFailErase)
            {
                l->failed = 1;
                AmdStartBusy(l, 0xFF, 1 << 30, A_READ);
            }
            else
            {
                AmdStartBusy(l, 0xFF, 40 + lane * 7, A_READ);
            }
        }
        else
        {
            l->state = A_READ;
        }
        break;
    case A_BYPASS:
        if (cmd == 0xA0)
            l->state = A_BYPROG;
        else if (cmd == 0x90)
            l->state = A_BYEXIT;
        else
            Violation("AMD: unexpected command in unlock bypass", gbaOffset, value);
        break;
    case A_BYEXIT:
        if (cmd == 0x00)
            l->state = A_READ;
        else
            Violation("AMD: bad unlock bypass exit", gbaOffset, value);
        break;
    }
}

static uint8_t AmdRead(int lane, uint32_t gbaOffset)
{
    struct AmdLane *l = &sLanes[lane];
    uint32_t addr = gbaOffset >> 1;
    uint8_t array = sRom[gbaOffset + (l->shift ? 1 : 0)];

    switch (l->state)
    {
    case A_BUSY:
        if (!l->failed && --l->busyReads <= 0)
        {
            l->state = l->afterBusy;
            sEraseInProgress = -1;
            return array;
        }
        l->toggle ^= 0x40;
        return LaneOut(l, (uint8_t)((~l->expect & 0x80) | l->toggle | (l->failed ? 0x20 : 0)));
    case A_AUTOSEL:
        if ((addr & 0xFF) == 0)
            return LaneOut(l, l->manufacturer);
        if ((addr & 0xFF) == 1)
            return LaneOut(l, (uint8_t)(l->x8 ? l->device : (l->shift ? l->device >> 8 : l->device)));
        return 0;
    case A_CFI:
    {
        static const uint8_t qry[] = {'Q', 'R', 'Y'};
        uint32_t index = l->x8 ? addr / 2 : addr;
        if (sAnswersCfiX16)
        {
            index = addr;
            if (index >= 0x10 && index <= 0x12)
                return LaneOut(l, qry[index - 0x10]);
            return 0;
        }
        if (l->x8 && (addr & 1))
            return 0;
        // x8 chips side by side: each holds half of every bus block.
        return LaneOut(l, CfiItem(index, 0, SIM_ROM_SIZE / sNumLanes, ReportedBlock() / sNumLanes, 0));
    }
    default:
        return array;
    }
}

// ------------------------------------------------------------------ Intel

enum { I_ARRAY, I_STATUS, I_ID, I_CFI };
enum { P_NONE, P_LOCK, P_ERASE, P_WORD, P_BUFCOUNT, P_BUFDATA, P_BUFCONFIRM };

static struct
{
    int mode;
    int pending;
    uint8_t sr;
    int busyReads;
    int swapped;
    int mlc;            // 1 KiB regions programmed once, buffered programming only
    uint8_t manufacturer;
    uint16_t device;
    uint8_t locked[SIM_ROM_SIZE / 0x10000];
    uint32_t bufStart;
    int bufCount;
    int bufIndex;
    uint32_t bufAddr[1024];
    uint16_t bufData[1024];
} sIntel;

static uint32_t BlockIndex(uint32_t offset) { return offset / 0x10000; }

static void IntelSetLock(uint32_t offset, int locked)
{
    uint32_t start = offset & ~(sEraseBlock - 1);
    uint32_t i;
    for (i = start; i < start + sEraseBlock; i += 0x10000)
        sIntel.locked[BlockIndex(i)] = locked;
}

static void IntelWrite(uint32_t gbaOffset, uint16_t value, int *programmed, int *erased)
{
    uint16_t chip = sIntel.swapped ? Swap01(value) : value;
    uint8_t cmd = (uint8_t)chip;

    if (sIntel.busyReads > 0)
    {
        Violation("Intel: write while busy", gbaOffset, value);
        return;
    }

    switch (sIntel.pending)
    {
    case P_LOCK:
        sIntel.pending = P_NONE;
        if (cmd == 0xD0)
            IntelSetLock(gbaOffset, 0);
        else if (cmd == 0x01)
            IntelSetLock(gbaOffset, 1);
        else
            Violation("Intel: bad lock command", gbaOffset, value);
        sIntel.mode = I_STATUS;
        return;
    case P_ERASE:
        sIntel.pending = P_NONE;
        sIntel.mode = I_STATUS;
        if (cmd != 0xD0)
        {
            sIntel.sr |= 0x30;
            Violation("Intel: erase without confirm", gbaOffset, value);
            return;
        }
        *erased = 1;
        if (sIntel.locked[BlockIndex(gbaOffset)])
        {
            sIntel.sr |= 0x22;
            Violation("Intel: erase of a locked block", gbaOffset, value);
            return;
        }
        if (sFailErase)
        {
            sIntel.sr |= 0x20;
            sFailErase = 0;
            sIntel.busyReads = 5;
            return;
        }
        EraseBlockAt(gbaOffset);
        sStats.erases--; // counted by caller
        sIntel.busyReads = 30;
        sEraseInProgress = gbaOffset & ~(sEraseBlock - 1);
        return;
    case P_WORD:
        sIntel.pending = P_NONE;
        sIntel.mode = I_STATUS;
        *programmed = 1;
        if (sIntel.mlc)
            Violation("Intel MLC: word program", gbaOffset, value);
        if (sIntel.locked[BlockIndex(gbaOffset)])
        {
            sIntel.sr |= 0x12;
            Violation("Intel: program of a locked block", gbaOffset, value);
            return;
        }
        if (sFailProgram)
        {
            sIntel.sr |= 0x10;
            sIntel.busyReads = 2;
            return;
        }
        if (!ProgramWord(gbaOffset, value) && value != 0xFFFF)
            Violation("Intel: programming a word that is not erased", gbaOffset, value);
        sIntel.busyReads = 2;
        return;
    case P_BUFCOUNT:
        sIntel.bufCount = chip + 1;
        sIntel.bufIndex = 0;
        sIntel.bufStart = gbaOffset;
        if (sIntel.bufCount > 512)
            Violation("Intel: buffer too large", gbaOffset, value);
        sIntel.pending = P_BUFDATA;
        return;
    case P_BUFDATA:
        if (sIntel.bufIndex < 1024)
        {
            sIntel.bufAddr[sIntel.bufIndex] = gbaOffset;
            sIntel.bufData[sIntel.bufIndex] = value;
        }
        if (++sIntel.bufIndex == sIntel.bufCount)
            sIntel.pending = P_BUFCONFIRM;
        return;
    case P_BUFCONFIRM:
    {
        int i;
        uint32_t region = sIntel.bufStart & ~0x3FF;

        sIntel.pending = P_NONE;
        sIntel.mode = I_STATUS;
        *programmed = 1;
        if (cmd != 0xD0)
        {
            sIntel.sr |= 0x30;
            Violation("Intel: buffer without confirm", gbaOffset, value);
            return;
        }
        if (sIntel.locked[BlockIndex(sIntel.bufStart)])
        {
            sIntel.sr |= 0x12;
            Violation("Intel: buffer program of a locked block", gbaOffset, value);
            return;
        }
        if (sIntel.mlc)
        {
            for (i = 0; i < 0x400; i += 2)
            {
                if (RomWord(region + i) != 0xFFFF)
                {
                    Violation("Intel MLC: region programmed twice", region, 0);
                    break;
                }
            }
        }
        for (i = 0; i < sIntel.bufCount; i++)
        {
            if ((sIntel.bufAddr[i] & ~0x3FF) != region)
                Violation("Intel: buffer crosses a 1 KiB region", sIntel.bufAddr[i], 0);
            if (!ProgramWord(sIntel.bufAddr[i], sIntel.bufData[i]) && sIntel.bufData[i] != 0xFFFF && !sIntel.mlc)
                Violation("Intel: programming a word that is not erased", sIntel.bufAddr[i], sIntel.bufData[i]);
        }
        if (sFailProgram)
            sIntel.sr |= 0x10;
        sIntel.busyReads = 4;
        return;
    }
    }

    switch (cmd)
    {
    case 0xFF: sIntel.mode = I_ARRAY; break;
    case 0x70: sIntel.mode = I_STATUS; break;
    case 0x50: sIntel.sr = 0x80; break;
    case 0x90: sIntel.mode = I_ID; break;
    case 0x98: sIntel.mode = I_CFI; break;
    case 0x60: sIntel.pending = P_LOCK; break;
    case 0x20: sIntel.pending = P_ERASE; break;
    case 0x40:
    case 0x10: sIntel.pending = P_WORD; break;
    case 0xE8:
    case 0xE9: sIntel.pending = P_BUFCOUNT; sIntel.mode = I_STATUS; break;
    default:
        sIntel.sr |= 0x30;
        sIntel.mode = I_STATUS;
        Violation("Intel: unknown command", gbaOffset, value);
        break;
    }
}

static uint16_t IntelRead(uint32_t gbaOffset)
{
    uint16_t chip;

    if (sIntel.busyReads > 0)
    {
        if (--sIntel.busyReads == 0)
            sEraseInProgress = -1;
        chip = sIntel.sr & 0x7F;
    }
    else if (sIntel.mode == I_STATUS)
    {
        chip = sIntel.sr;
    }
    else if (sIntel.mode == I_ID)
    {
        uint32_t word = (gbaOffset & (sEraseBlock - 1)) >> 1;
        if (gbaOffset == 0)
            chip = sIntel.manufacturer;
        else if (gbaOffset == 2)
            chip = sIntel.device;
        else if (word == 1 || word == 2)
            chip = sIntel.locked[BlockIndex(gbaOffset)]; // block lock status
        else
            chip = 0;
    }
    else if (sIntel.mode == I_CFI)
    {
        chip = CfiItem(gbaOffset >> 1, 1, SIM_ROM_SIZE, ReportedBlock(), sBoard == SIM_INTEL_WORD);
    }
    else
    {
        return RomWord(gbaOffset);
    }

    return sIntel.swapped ? Swap01(chip) : chip;
}

// ------------------------------------------------------------------ hooks

uint16_t HostRomRead16(uint32_t offset)
{
    offset &= SIM_ROM_SIZE - 2;

    switch (sBoard)
    {
    case SIM_INTEL_BUFFERED:
    case SIM_INTEL_WORD:
    case SIM_INTEL_UNKNOWN:
        return IntelRead(offset);
    case SIM_AMD_SWAPPED:
    {
        // One x16 chip: status and IDs come from the chip, array data is 16 bits.
        int before = sLanes[0].state;
        uint16_t lo = AmdRead(0, offset);
        uint16_t hi = 0;
        int after = sLanes[0].state;

        if (before == A_READ || before == A_BYPASS || (before == A_BUSY && after != A_BUSY))
            hi = sRom[offset + 1];
        else if (before == A_AUTOSEL && ((offset >> 1) & 0xFF) == 1)
            hi = sLanes[0].device >> 8;
        return lo | (hi << 8);
    }
    case SIM_AMD_DUAL:
    case SIM_AMD_X16:
        return AmdRead(0, offset) | (AmdRead(1, offset) << 8);
    default:
        return RomWord(offset);
    }
}

void HostRomWrite16(uint32_t offset, uint16_t value)
{
    int programmed = 0, erased = 0;

    offset &= SIM_ROM_SIZE - 2;

    switch (sBoard)
    {
    case SIM_INTEL_BUFFERED:
    case SIM_INTEL_WORD:
    case SIM_INTEL_UNKNOWN:
        IntelWrite(offset, value, &programmed, &erased);
        break;
    case SIM_AMD_SWAPPED:
    {
        // One x16 chip: commands come from the low byte, data is 16 bits.
        struct AmdLane *l = &sLanes[0];
        int wasProg = (l->state == A_PROG || l->state == A_BYPROG);
        AmdWrite(0, offset, value, &programmed, &erased);
        if (wasProg && programmed)
        {
            uint8_t oldHi = sRom[offset + 1];
            if (oldHi != 0xFF && (value >> 8) != 0xFF)
                Violation("AMD: programming a byte that is not erased", offset, value);
            sRom[offset + 1] = oldHi & (value >> 8);
        }
        if (erased && !sFailErase)
        {
            EraseBlockAt(offset);
            sEraseInProgress = offset & ~(sEraseBlock - 1);
        }
        break;
    }
    case SIM_AMD_DUAL:
    case SIM_AMD_X16:
        AmdWrite(1, offset, value, &programmed, &erased);
        AmdWrite(0, offset, value, &programmed, &erased);
        if (erased && !sFailErase)
        {
            EraseBlockAt(offset);
            sEraseInProgress = offset & ~(sEraseBlock - 1);
        }
        break;
    default:
        sStats.strayWrites++;
        break;
    }

    if (erased)
    {
        sStats.erases += (sBoard == SIM_INTEL_BUFFERED || sBoard == SIM_INTEL_WORD || sBoard == SIM_INTEL_UNKNOWN) ? 1 : 0;
        sFailErase = 0;
    }
    if (programmed)
    {
        sStats.programs++;
        sFailProgram = 0;
    }
}

uint8_t HostSramRead8(uint32_t offset)
{
    return sSram[(sBank << 16) | (offset & 0xFFFF)];
}

void HostSramWrite8(uint32_t offset, uint8_t value)
{
    sSram[(sBank << 16) | (offset & 0xFFFF)] = value;
}

void HostBankWrite8(uint8_t value)
{
    int ok = (value == 0 || value == 1);

    if (sBoard == SIM_AMD_SWAPPED && (value == 0xEE || value == 0xEF))
        ok = 1;
    if (!ok)
        sStats.badBankWrites++;
    sBank = value & 1;
}

void SimInit(enum SimBoard board, uint32_t seed)
{
    uint32_t i, x = seed;

    if (!sRom)
        sRom = malloc(SIM_ROM_SIZE);
    for (i = 0; i < SIM_ROM_SIZE; i++)
    {
        x = x * 1103515245 + 12345;
        sRom[i] = x >> 16;
    }
    // Cart header: branch over the header, as on a real cart.
    sRom[0] = 0x2E; sRom[1] = 0x00; sRom[2] = 0x00; sRom[3] = 0xEA;
    memcpy(sRom + 0xA0, "POKEMON EMERBPEE01", 18);
    for (i = 0; i < SIM_SRAM_SIZE; i++)
    {
        x = x * 1103515245 + 12345;
        sSram[i] = x >> 16;
    }

    memset(&sStats, 0, sizeof(sStats));
    memset(sLanes, 0, sizeof(sLanes));
    memset(&sIntel, 0, sizeof(sIntel));
    sViolation[0] = 0;
    sFailErase = sFailProgram = 0;
    sAnswersCfiX16 = 0;
    sCfiBlock = 0;
    sEraseInProgress = -1;
    sRandom = seed;
    sBank = 0;
    sBoard = board;

    switch (board)
    {
    case SIM_AMD_SWAPPED:
        sNumLanes = 1;
        sLanes[0].swapped = 1;
        sLanes[0].manufacturer = 0x01;
        sLanes[0].device = 0x227E; // reads as 0x227D through the D0/D1 swap
        sEraseBlock = 0x10000;
        break;
    case SIM_AMD_DUAL:
    case SIM_AMD_X16:
        sNumLanes = 2;
        sLanes[0].swapped = 1;
        sLanes[0].x8 = 1;
        sLanes[1].x8 = 1;
        sLanes[1].shift = 8;
        sLanes[0].manufacturer = sLanes[1].manufacturer = 0xC2;
        sLanes[0].device = sLanes[1].device = 0x7E;
        sEraseBlock = 0x20000; // 64 KiB sectors on each x8 chip
        if (board == SIM_AMD_X16)
        {
            sAnswersCfiX16 = 1;
            sLanes[0].x8 = sLanes[1].x8 = 0;
        }
        break;
    case SIM_INTEL_BUFFERED:
        sIntel.swapped = 1;
        sIntel.mlc = 1;
        sIntel.manufacturer = 0x89;
        sIntel.device = 0x887E; // reads as 0x887D through the D0/D1 swap
        sIntel.sr = 0x80;
        memset(sIntel.locked, 1, sizeof(sIntel.locked));
        sEraseBlock = 0x20000;
        break;
    case SIM_INTEL_WORD:
        sIntel.manufacturer = 0x89;
        sIntel.device = 0x8815;
        sIntel.sr = 0x80;
        memset(sIntel.locked, 1, sizeof(sIntel.locked));
        sEraseBlock = 0x10000;
        break;
    case SIM_INTEL_UNKNOWN:
        sIntel.manufacturer = 0x89;
        sIntel.device = 0x1234;
        sIntel.sr = 0x80;
        memset(sIntel.locked, 1, sizeof(sIntel.locked));
        sEraseBlock = 0x20000;
        break;
    default:
        sEraseBlock = 0x10000;
        break;
    }
}

// Power loss and power-on: chips back to read mode (Intel blocks locked
// again), an erase that was still running leaves its block in a random
// half-erased state, and the SRAM (no battery) loses its contents.
void SimPowerCycle(void)
{
    uint32_t i;
    int lane;

    if (sEraseInProgress >= 0)
    {
        for (i = 0; i < sEraseBlock; i++)
            sRom[sEraseInProgress + i] |= Random8();
        sEraseInProgress = -1;
    }
    for (lane = 0; lane < 2; lane++)
    {
        sLanes[lane].state = A_READ;
        sLanes[lane].failed = 0;
        sLanes[lane].busyReads = 0;
    }
    sIntel.mode = I_ARRAY;
    sIntel.pending = P_NONE;
    sIntel.busyReads = 0;
    sIntel.sr = 0x80;
    if (sBoard == SIM_INTEL_BUFFERED || sBoard == SIM_INTEL_WORD || sBoard == SIM_INTEL_UNKNOWN)
        memset(sIntel.locked, 1, sizeof(sIntel.locked));
    for (i = 0; i < SIM_SRAM_SIZE; i++)
        sSram[i] = Random8();
    sBank = 1;
    sFailErase = sFailProgram = 0;
}
