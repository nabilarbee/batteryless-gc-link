// Locates a FLASH1M-format Pokémon save inside cart ROM.
// This file has no hardware access so it can also be compiled on the host
// (REPRO_HOST_TEST) and tested against real ROM dumps.

#include "repro_types.h"
#include "repro_flash.h"

#define SECTORS_PER_SLOT 14

#define SLOT_EMPTY    0
#define SLOT_COMPLETE 1
#define SLOT_PARTIAL  2

// Sector footer layout (see struct SaveSector in libpmagb/save.c)
#define FOOTER_ID        0xFF4
#define FOOTER_SIGNATURE 0xFF8
#define FOOTER_COUNTER   0xFFC

static u16 RomRead16(const u8 *rom, u32 offset)
{
    return *(const vu16 *)(rom + offset);
}

static u32 RomRead32(const u8 *rom, u32 offset)
{
    return (u32)RomRead16(rom, offset) | ((u32)RomRead16(rom, offset + 2) << 16);
}

// A slot is complete when all 14 sectors carry the signature, their ids are
// exactly 0..13 and they share one save counter.
static u32 CheckSlot(const u8 *rom, u32 slotStart, u32 *counterOut)
{
    u32 i;
    u32 idsSeen = 0;
    u32 numSigned = 0;
    u32 counter = 0;
    bool32 countersMatch = TRUE;

    for (i = 0; i < SECTORS_PER_SLOT; i++)
    {
        u32 sector = slotStart + i * REPRO_SECTOR_SIZE;

        if (RomRead32(rom, sector + FOOTER_SIGNATURE) == REPRO_SIGNATURE)
        {
            u32 id = RomRead16(rom, sector + FOOTER_ID);
            u32 sectorCounter = RomRead32(rom, sector + FOOTER_COUNTER);

            if (numSigned == 0)
                counter = sectorCounter;
            else if (sectorCounter != counter)
                countersMatch = FALSE;

            if (id < SECTORS_PER_SLOT)
                idsSeen |= 1 << id;

            numSigned++;
        }
    }

    if (numSigned == 0)
        return SLOT_EMPTY;

    if (numSigned == SECTORS_PER_SLOT && idsSeen == (1 << SECTORS_PER_SLOT) - 1 && countersMatch)
    {
        *counterOut = counter;
        return SLOT_COMPLETE;
    }

    return SLOT_PARTIAL;
}

// True when [half, 2*half) reads back the same as [0, half) at a spread of
// sample points, meaning the chip is not larger than 'half'.
static bool32 IsMirror(const u8 *rom, u32 half)
{
    u32 i;

    // The cart header is never blank, so a blank upper half fails here.
    for (i = 0; i < 0xC0; i += 2)
    {
        if (RomRead16(rom, i) != RomRead16(rom, half + i))
            return FALSE;
    }

    for (i = 1; i < 64; i++)
    {
        u32 offset = i * (half / 64) + 0x1F2;

        if (RomRead16(rom, offset) != RomRead16(rom, half + offset))
            return FALSE;
    }

    return TRUE;
}

// Readable size of cart ROM: 32 MiB, reduced while the upper half mirrors the lower.
u32 ReproGetRomSize(const u8 *rom)
{
    u32 size = REPRO_ROM_MAX_SIZE;

    while (size > 0x400000 && IsMirror(rom, size / 2))
        size /= 2;

    return size;
}

bool32 ReproScanForSave(const u8 *rom, u32 romSize, struct ReproScanResult *result)
{
    u32 base;
    bool32 found = FALSE;

    result->saveOffset = 0;
    result->newestCounter = 0;
    result->numCandidates = 0;
    result->completeSlots = 0;

    for (base = 0; base + REPRO_SAVE_SIZE <= romSize; base += REPRO_SCAN_STEP)
    {
        u32 counterA = 0;
        u32 counterB = 0;
        u32 slotA = CheckSlot(rom, base, &counterA);
        u32 slotB = CheckSlot(rom, base + SECTORS_PER_SLOT * REPRO_SECTOR_SIZE, &counterB);
        u32 newest;
        u8 completeSlots = 0;

        if (slotA == SLOT_COMPLETE)
            completeSlots |= 1;
        if (slotB == SLOT_COMPLETE)
            completeSlots |= 2;

        if (completeSlots == 0)
            continue;

        if (completeSlots == 3)
            newest = (counterA > counterB) ? counterA : counterB;
        else if (completeSlots == 1)
            newest = counterA;
        else
            newest = counterB;

        result->numCandidates++;

        // With several candidates keep the most recent save.
        if (!found || newest > result->newestCounter)
        {
            result->saveOffset = base;
            result->newestCounter = newest;
            result->completeSlots = completeSlots;
            found = TRUE;
        }
    }

    return found;
}
