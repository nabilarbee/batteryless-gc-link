// Batteryless save fix for Pokemon XD's GBA link programs.
//
// XD sends its own GBA program, and Emerald runs it as sent. That program
// looks for a flash save chip, which batteryless repro carts don't have. The
// cart's link hook (linkhook.c) copies this code into free EWRAM and
// redirects the program's calls to IdentifyFlash, ReadFlash,
// VerifyFlashSector(NBytes) and its save step here. The save is then read
// from and written to the cart's ROM flash by the same backend as the
// Colosseum repro build (payload/src/repro), which is linked in unchanged.

#include "repro_types.h"
#include "gba/flash_internal.h"
#include "repro_flash.h"
#include "xd_params.h"

struct XdParams gXdParams;

u16 Xd_IdentifyFlash(void)
{
    // A save image inside cart ROM means a batteryless repro cart.
    if (ReproFlash_Detect())
        return 0;
    return ((u16 (*)(void))gXdParams.identifyFlash)();
}

void Xd_ReadFlash(u16 sectorNum, u32 offset, void *dest, u32 size)
{
    if (gReproActive)
    {
        ReproFlash_Read(sectorNum, offset, dest, size);
        return;
    }
    ((void (*)(u16, u32, void *, u32))gXdParams.readFlash)(sectorNum, offset, dest, size);
}

u32 Xd_VerifyFlashSector(u16 sectorNum, u8 *src)
{
    if (gReproActive)
        return ReproFlash_Verify(sectorNum, src, REPRO_SECTOR_SIZE);
    return ((u32 (*)(u16, u8 *))gXdParams.verifyFlashSector)(sectorNum, src);
}

u32 Xd_VerifyFlashSectorNBytes(u16 sectorNum, u8 *src, u32 n)
{
    if (gReproActive)
        return ReproFlash_Verify(sectorNum, src, n);
    return ((u32 (*)(u16, u8 *, u32))gXdParams.verifyFlashSectorNBytes)(sectorNum, src, n);
}

// The link code's save step: SavePhase(0) writes the save except one byte,
// SavePhase(1) writes that byte. As in the Colosseum repro build, each phase
// is written to cart ROM before its result goes back to the GameCube.
bool32 Xd_SavePhase(s32 phase)
{
    bool32 failed = ((bool32 (*)(s32))gXdParams.savePhase)(phase);

    if (!failed && gReproActive)
    {
        ((void (*)(void))gXdParams.soundVSyncOff)();
        if (ReproFlash_Commit() != 0)
            failed = TRUE;
        ((void (*)(void))gXdParams.soundVSyncOn)();
    }
    return failed;
}
