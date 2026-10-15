// AgbFlash backend for batteryless repro carts (see include/repro_flash.h).
//
// Reads come straight from the save area in cart ROM until the first write.
// The first write identifies the board, copies the save area into the cart's
// banked SRAM and from then on the AgbFlash API works on that copy.
// ReproFlash_Commit writes the copy back to ROM; the link code calls it at
// the end of each phase of a save, before reporting the result.

#include "gba/gba.h"
#include "gba/flash_internal.h"
#include "repro_flash.h"
#include "repro_cart.h"
#include "repro_image.h"

#define SESSION_NONE   0
#define SESSION_READY  1
#define SESSION_FAILED 2

u8 gReproActive = FALSE;
u32 gReproSaveOffset = 0;
u8 gReproSession = SESSION_NONE;
u8 gReproCartType = REPRO_CART_UNKNOWN;
u16 gReproLastError = 0;

static const u16 sReproMaxTime[] =
{
    10, 65469, TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_256CLK,
    10, 65469, TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_256CLK,
    2000, 65469, TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_256CLK,
    2000, 65469, TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_256CLK,
};

// Same geometry as the 128 KiB flash chips, so save.c needs no changes.
static const struct FlashType sReproFlashType =
{
    131072, // ROM size
    {
        4096, // sector size
          12, // bit shift to multiply by sector size (4096 == 1 << 12)
          32, // number of sectors
           0  // appears to be unused
    },
    { 3, 1 }, // wait state setup data
    { { 0x00, 0x00 } } // no chip ID
};

static bool32 InRange(u16 sectorNum, u32 offset, u32 size)
{
    return sectorNum < REPRO_NUM_SECTORS && offset <= REPRO_SECTOR_SIZE && size <= REPRO_SECTOR_SIZE - offset;
}

static u32 ImageOffset(u16 sectorNum, u32 offset)
{
    return ((u32)sectorNum << 12) + offset;
}

static const vu8 *RomPtr(u16 sectorNum, u32 offset)
{
    return (const vu8 *)(REPRO_ROM_START + gReproSaveOffset + ImageOffset(sectorNum, offset));
}

static bool32 BeginWriteSession(void)
{
    const struct ReproCart *cart;
    u16 result;

    if (gReproSession == SESSION_READY)
        return TRUE;
    if (gReproSession == SESSION_FAILED)
        return FALSE;

    cart = ReproCart_Probe();
    gReproCartType = cart->type;

    // Erases start at the save offset and cover cart->eraseStride bytes, so
    // the save must start on that boundary or data before it would be erased.
    if (!cart->supported || (gReproSaveOffset & (cart->eraseStride - 1)) != 0)
        result = REPRO_ERR_UNSUPPORTED;
    else
        result = ReproImage_Load(gReproSaveOffset);

    if (result != REPRO_OK)
    {
        gReproLastError = result;
        gReproSession = SESSION_FAILED;
        return FALSE;
    }

    gReproSession = SESSION_READY;
    return TRUE;
}

void ReproFlash_Read(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    const vu8 *src;

    if (!InRange(sectorNum, offset, size))
        return;

    if (gReproSession == SESSION_READY)
    {
        ReproImage_Read(ImageOffset(sectorNum, offset), dest, size);
        return;
    }

    src = RomPtr(sectorNum, offset);
    while (size-- != 0)
        *dest++ = *src++;
}

// Returns 0 when equal, otherwise nonzero (like VerifyFlashSector).
u32 ReproFlash_Verify(u16 sectorNum, const u8 *src, u32 size)
{
    const vu8 *tgt;

    if (!InRange(sectorNum, 0, size))
        return REPRO_ERR_RANGE;

    if (gReproSession == SESSION_READY)
        return ReproImage_Compare(ImageOffset(sectorNum, 0), src, size);

    tgt = RomPtr(sectorNum, 0);
    while (size-- != 0)
    {
        if (*tgt++ != *src++)
            return (u32)(tgt - 1);
    }

    return 0;
}

u16 ReproFlash_ProgramByte(u16 sectorNum, u32 offset, u8 data)
{
    if (!InRange(sectorNum, offset, 1))
        return REPRO_ERR_RANGE;
    if (!BeginWriteSession())
        return REPRO_ERR_READ_ONLY;

    ReproImage_Write(ImageOffset(sectorNum, offset), &data, 1);
    return 0;
}

u16 ReproFlash_ProgramSector(u16 sectorNum, void *src)
{
    if (!InRange(sectorNum, 0, REPRO_SECTOR_SIZE))
        return REPRO_ERR_RANGE;
    if (!BeginWriteSession())
        return REPRO_ERR_READ_ONLY;

    ReproImage_Write(ImageOffset(sectorNum, 0), src, REPRO_SECTOR_SIZE);
    return 0;
}

u16 ReproFlash_EraseSector(u16 sectorNum)
{
    if (!InRange(sectorNum, 0, REPRO_SECTOR_SIZE))
        return REPRO_ERR_RANGE;
    if (!BeginWriteSession())
        return REPRO_ERR_READ_ONLY;

    ReproImage_Fill(ImageOffset(sectorNum, 0), 0xFF, REPRO_SECTOR_SIZE);
    return 0;
}

u16 ReproFlash_EraseChip(void)
{
    u16 i;

    if (!BeginWriteSession())
        return REPRO_ERR_READ_ONLY;

    for (i = 0; i < REPRO_NUM_SECTORS; i++)
        ReproImage_Fill(ImageOffset(i, 0), 0xFF, REPRO_SECTOR_SIZE);
    return 0;
}

u16 ReproFlash_WaitForWrite(u8 phase, u8 *addr, u8 lastData)
{
    return 0;
}

// Writes the working copy back to cart ROM. Returns 0 on success.
u16 ReproFlash_Commit(void)
{
    u16 result;

    if (!gReproActive || gReproSession != SESSION_READY)
        return 0;

    result = ReproImage_Commit(ReproCart_Get(gReproCartType), gReproSaveOffset);
    if (result != REPRO_OK)
        gReproLastError = result;

    return result;
}

// Called by IdentifyFlash before probing for a real flash chip.
bool32 ReproFlash_Detect(void)
{
    struct ReproScanResult result;
    const u8 *rom = (const u8 *)REPRO_ROM_START;

    gReproActive = FALSE;
    gReproSession = SESSION_NONE;

    if (!ReproScanForSave(rom, ReproGetRomSize(rom), &result))
        return FALSE;

    gReproSaveOffset = result.saveOffset;
    gReproActive = TRUE;

    ProgramFlashByte = ReproFlash_ProgramByte;
    ProgramFlashSector = ReproFlash_ProgramSector;
    EraseFlashChip = ReproFlash_EraseChip;
    EraseFlashSector = ReproFlash_EraseSector;
    WaitForFlashWrite = ReproFlash_WaitForWrite;
    gFlashMaxTime = sReproMaxTime;
    gFlash = &sReproFlashType;

    return TRUE;
}
