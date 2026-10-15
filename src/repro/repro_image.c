// SRAM working copy of the save and the commit to ROM.
//
// The cart's own boot code copies the ROM save area into SRAM at power-on,
// and its save routine writes SRAM back to ROM. This file does the same for
// the link program: the AgbFlash API edits the SRAM copy, and a commit
// writes the difference back to the cart's ROM flash.
//
// A commit never programs a unit that already holds data: it either
// programs units that are still erased, or erases the whole save area and
// reprograms it. The 4 KiB sector whose signature is still pending (the
// first phase of the link save) is left erased around its signature until
// the second phase sets the signature, so phase two only programs erased
// flash. This keeps every chip type safe, including ones that cannot be
// reprogrammed without an erase.

#include "repro_types.h"
#include "repro_hw.h"
#include "repro_cart.h"
#include "repro_flash.h"
#include "repro_image.h"

#define SECTORS_PER_SLOT 14

static const u8 sBootBanks[2] = {REPRO_BOOT_BANK0, REPRO_BOOT_BANK1};

// Bank register values in use: the boot code's values while the link program
// edits the image, the board's flush values during a commit (as the cart's
// own code does).
static const u8 *sBanks = sBootBanks;

// ---------------------------------------------------------------- image access

static void SelectBank(u32 offset)
{
    ReproSram_SelectBank(offset >> 16, sBanks);
}

// Reads within one bank (callers never cross a 64 KiB boundary).
static u8 ImageByte(u32 offset)
{
    return SRAM_READ8(offset & 0xFFFF);
}

static u16 ImageWord(u32 offset)
{
    return ImageByte(offset) | (ImageByte(offset + 1) << 8);
}

u16 ReproImage_Load(u32 saveOffset)
{
    u32 bank, i;

    ReproSram_ForgetBank();
    sBanks = sBootBanks;

    // The two banks must be independent 64 KiB pages.
    ReproSram_Write8(0x00000, 0xA5, sBootBanks);
    ReproSram_Write8(0x10000, 0x5A, sBootBanks);
    if (ReproSram_Read8(0x00000, sBootBanks) != 0xA5 || ReproSram_Read8(0x10000, sBootBanks) != 0x5A)
        return REPRO_ERR_SRAM;

    for (bank = 0; bank < 2; bank++)
    {
        u32 base = bank << 16;

        ReproSram_SelectBank(bank, sBootBanks);
        for (i = 0; i < REPRO_SRAM_BANK_SIZE; i += 2)
        {
            u16 word = ROM_READ16(saveOffset + base + i);
            SRAM_WRITE8(i, word);
            SRAM_WRITE8(i + 1, word >> 8);
        }
        for (i = 0; i < REPRO_SRAM_BANK_SIZE; i += 2)
        {
            u16 word = ROM_READ16(saveOffset + base + i);
            if (SRAM_READ8(i) != (u8)word || SRAM_READ8(i + 1) != (u8)(word >> 8))
                return REPRO_ERR_SRAM;
        }
    }

    ReproSram_SelectBank(0, sBootBanks);
    return REPRO_OK;
}

void ReproImage_Read(u32 offset, u8 *dest, u32 size)
{
    SelectBank(offset);
    while (size-- != 0)
        *dest++ = ImageByte(offset++);
}

// Returns 0 when equal, otherwise 1 + the index of the first difference.
u32 ReproImage_Compare(u32 offset, const u8 *src, u32 size)
{
    u32 i;

    SelectBank(offset);
    for (i = 0; i < size; i++)
    {
        if (ImageByte(offset + i) != src[i])
            return i + 1;
    }

    return 0;
}

void ReproImage_Write(u32 offset, const u8 *src, u32 size)
{
    SelectBank(offset);
    while (size-- != 0)
        SRAM_WRITE8((offset++) & 0xFFFF, *src++);
}

void ReproImage_Fill(u32 offset, u8 value, u32 size)
{
    SelectBank(offset);
    while (size-- != 0)
        SRAM_WRITE8((offset++) & 0xFFFF, value);
}

// ---------------------------------------------------------------- commit

#define UNIT_SAME        0
#define UNIT_PROGRAM     1 // ROM unit erased, image unit has data
#define UNIT_NEEDS_ERASE 2

#define FOOTER_ID        0xFF4
#define FOOTER_SIGNATURE 0xFF8
#define FOOTER_COUNTER   0xFFC

// First phase of a link save: the whole slot is written except the first
// byte of one signature, which stays 0xFF until the second phase.
static bool32 IsPendingSignature(u32 sigOffset)
{
    return ImageByte(sigOffset) == 0xFF
        && ImageByte(sigOffset + 1) == 0x20
        && ImageByte(sigOffset + 2) == 0x01
        && ImageByte(sigOffset + 3) == 0x08;
}

// Bank must already be selected for 'unit'.
static bool32 UnitIsDeferred(const struct ReproCart *cart, u32 unit)
{
    u32 sigOffset = (unit & ~(REPRO_SECTOR_SIZE - 1)) + FOOTER_SIGNATURE;

    if (sigOffset < unit || sigOffset >= unit + cart->programUnit)
        return FALSE;

    return IsPendingSignature(sigOffset);
}

static u32 ClassifyUnit(const struct ReproCart *cart, u32 saveOffset, u32 unit, bool32 deferred)
{
    u32 i;
    bool32 romErased = TRUE;
    bool32 differs = FALSE;

    for (i = 0; i < cart->programUnit; i += 2)
    {
        u16 rom = ROM_READ16(saveOffset + unit + i);
        u16 want = deferred ? 0xFFFF : ImageWord(unit + i);

        if (rom != 0xFFFF)
            romErased = FALSE;
        if (rom != want)
            differs = TRUE;
    }

    if (!differs)
        return UNIT_SAME;

    return romErased ? UNIT_PROGRAM : UNIT_NEEDS_ERASE;
}

static bool32 UnitIsBlank(const struct ReproCart *cart, u32 unit)
{
    u32 i;

    for (i = 0; i < cart->programUnit; i += 2)
    {
        if (ImageWord(unit + i) != 0xFFFF)
            return FALSE;
    }

    return TRUE;
}

// Save counter of a complete slot in the image, or -1.
static s32 SlotCounter(u32 firstSector)
{
    u32 i;
    u32 idsSeen = 0;
    u32 counter = 0;

    for (i = 0; i < SECTORS_PER_SLOT; i++)
    {
        u32 sector = (firstSector + i) * REPRO_SECTOR_SIZE;
        u32 sig, sectorCounter;

        SelectBank(sector);
        sig = ImageWord(sector + FOOTER_SIGNATURE) | ((u32)ImageWord(sector + FOOTER_SIGNATURE + 2) << 16);
        if (sig != REPRO_SIGNATURE)
            return -1;
        sectorCounter = ImageWord(sector + FOOTER_COUNTER) | ((u32)ImageWord(sector + FOOTER_COUNTER + 2) << 16);
        if (i == 0)
            counter = sectorCounter;
        else if (sectorCounter != counter)
            return -1;
        idsSeen |= 1 << (ImageWord(sector + FOOTER_ID) & 0x1F);
    }

    if (idsSeen != (1 << SECTORS_PER_SLOT) - 1)
        return -1;

    return counter & 0x7FFFFFFF;
}

// Sector order for a full rewrite: the newest complete slot first, so that a
// valid save exists in ROM again as early as possible.
static void GetSectorOrder(u8 *order)
{
    s32 counterA = SlotCounter(0);
    s32 counterB = SlotCounter(SECTORS_PER_SLOT);
    u32 first = (counterB > counterA) ? SECTORS_PER_SLOT : 0;
    u32 second = SECTORS_PER_SLOT - first;
    u32 i, n = 0;

    for (i = 0; i < SECTORS_PER_SLOT; i++)
        order[n++] = first + i;
    for (i = 0; i < SECTORS_PER_SLOT; i++)
        order[n++] = second + i;
    for (i = SECTORS_PER_SLOT * 2; i < REPRO_NUM_SECTORS; i++)
        order[n++] = i;
}

// Programs every unit of one sector that should hold data.
static u16 ProgramSector(const struct ReproCart *cart, u32 saveOffset, u32 sector, bool32 afterErase)
{
    u32 unit;
    u32 start = sector * REPRO_SECTOR_SIZE;
    u16 result = REPRO_OK;

    for (unit = start; unit < start + REPRO_SECTOR_SIZE && result == REPRO_OK; unit += cart->programUnit)
    {
        bool32 deferred;
        bool32 program;

        SelectBank(unit);
        deferred = UnitIsDeferred(cart, unit);
        if (deferred)
            continue;

        if (afterErase)
            program = !UnitIsBlank(cart, unit);
        else
            program = ClassifyUnit(cart, saveOffset, unit, FALSE) == UNIT_PROGRAM;

        if (program)
            result = ReproCart_ProgramUnit(cart, saveOffset + unit, unit);
    }

    return result;
}

static u16 VerifyAll(const struct ReproCart *cart, u32 saveOffset)
{
    u32 unit;

    for (unit = 0; unit < REPRO_SAVE_SIZE; unit += cart->programUnit)
    {
        SelectBank(unit);
        if (ClassifyUnit(cart, saveOffset, unit, UnitIsDeferred(cart, unit)) != UNIT_SAME)
            return REPRO_ERR_VERIFY;
    }

    return REPRO_OK;
}

u16 ReproImage_Commit(const struct ReproCart *cart, u32 saveOffset)
{
    u32 unit, i, attempt;
    bool32 needErase = FALSE;
    bool32 anyChange = FALSE;
    u16 result = REPRO_OK;
    u8 order[REPRO_NUM_SECTORS];

    if (!cart->supported)
        return REPRO_ERR_UNSUPPORTED;

    sBanks = cart->bankValue;

    for (unit = 0; unit < REPRO_SAVE_SIZE; unit += cart->programUnit)
    {
        u32 status;

        SelectBank(unit);
        status = ClassifyUnit(cart, saveOffset, unit, UnitIsDeferred(cart, unit));
        if (status != UNIT_SAME)
            anyChange = TRUE;
        if (status == UNIT_NEEDS_ERASE)
            needErase = TRUE;
    }

    if (!anyChange)
        goto done;

    GetSectorOrder(order);

    for (attempt = 0; attempt < 2; attempt++)
    {
        result = REPRO_OK;

        if (needErase)
        {
            for (i = 0; i < REPRO_SAVE_SIZE && result == REPRO_OK; i += cart->eraseStride)
                result = ReproCart_EraseBlock(cart, saveOffset + i);
        }

        for (i = 0; i < REPRO_NUM_SECTORS && result == REPRO_OK; i++)
            result = ProgramSector(cart, saveOffset, order[i], needErase);

        if (result == REPRO_OK)
            result = VerifyAll(cart, saveOffset);

        if (result == REPRO_OK)
            break;

        // Retry once from a clean erase.
        needErase = TRUE;
    }

    ReproCart_ReadArrayMode(cart);

done:
    sBanks = sBootBanks;
    SelectBank(0);

    return result;
}
