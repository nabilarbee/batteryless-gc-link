// Emerald save writer.
//
// A GBA multiboot program, sent like the flasher, that writes a 128 KiB
// Emerald save file into the save area of a batteryless repro cart, where the
// cart's own code keeps the save (in ROM flash). make_save_writer.py builds it
// from the user's dump and the save file, typically one that extract_save.py
// pulled out of a dump and PKHeX edited.
//
// Safety design:
//  - nothing is written until the cart matches the dump outside the areas that
//    legitimately change, and the user confirms with L+R+A;
//  - only the save area is erased, by commands sent inside it, as the cart's
//    own save routine does (see savecore.c);
//  - the image carries the whole save, so if power is lost while writing,
//    sending it again finishes the job.

#include "repro_types.h"
#include "gba/io_reg.h"
#include "console.h"
#include "config.h"
#include "savecore.h"
#include "savewriter_config.h"

#define VERSION "v0.1"
#define ROM_ADDR(off) ((const void *)(0x08000000 + (off)))

extern u8 __image_end[];

static const struct SaveWriterConfig *sCfg;
static const u8 *sSave;

// The end of every run; the GBA is turned off from here.
void Finish(u32 code)
{
    for (;;)
        WaitVBlank();
}

static void Fail(const char *msg)
{
    Con_SetColor(COLOR_RED);
    Con_Print(msg);
    Con_SetColor(COLOR_WHITE);
    Finish(1);
}

static void WaitConfirm(void)
{
    u16 keys;
    u16 prev = 0;

    // Start+Select may still be held from booting into multiboot.
    while (ReadKeys())
        WaitVBlank();
    for (;;)
    {
        WaitVBlank();
        keys = ReadKeys();
        if ((keys & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (keys & KEY_A) && !(prev & KEY_A))
            return;
        prev = keys;
    }
}

static bool32 ConfigOk(void)
{
    sCfg = (const struct SaveWriterConfig *)(((u32)__image_end + 31) & ~31);
    if (sCfg->magic != SAVEWRITER_MAGIC || sCfg->version != SAVEWRITER_VERSION)
        return FALSE;
    if (sCfg->totalSize < sizeof(struct SaveWriterConfig) || (sCfg->totalSize & 31)
     || (u32)sCfg + sCfg->totalSize > 0x02040000)
        return FALSE;
    if (gHashWords((const u8 *)sCfg + 0x20, sCfg->totalSize - 0x20, HASH_BASIS) != sCfg->bodyHash)
        return FALSE;
    if (sCfg->numIdRanges > SAVEWRITER_ID_RANGES || sCfg->saveSize != SAVE_SIZE
     || sizeof(struct SaveWriterConfig) + sCfg->saveSize > sCfg->totalSize
     || (sCfg->saveAddr & (SECTOR_SIZE - 1)) || sCfg->areaStart > sCfg->saveAddr
     || sCfg->areaEnd < sCfg->saveAddr + sCfg->saveSize)
        return FALSE;
    sSave = (const u8 *)(sCfg + 1);
    return TRUE;
}

static bool32 IdentityOk(void)
{
    u32 h = HASH_BASIS;
    u32 i;

    for (i = 0; i < sCfg->numIdRanges; i++)
        h = gHashWords(ROM_ADDR(sCfg->idRanges[i][0]), sCfg->idRanges[i][1] - sCfg->idRanges[i][0], h);
    return h == sCfg->idHash;
}

static bool32 CartPresent(void)
{
    return *(vu32 *)0x080000AC == sCfg->gameCode;
}

static void WaitForCart(void)
{
    if (CartPresent())
        return;
    Con_Print("Insert the Emerald cart.\n");
    do
    {
        while (!CartPresent())
            WaitVBlank();
        WaitFrames(60);
    } while (!CartPresent());
}

void AgbMain(void)
{
    struct SaveInfo cartInfo, newInfo;
    const char *refusal;
    u16 rc;

    REG_WAITCNT = 0x4014; // the same as Emerald: ROM 3/1 wait states, SRAM 4
    Con_Init();
    Con_SetColor(COLOR_YELLOW);
    Con_Print("Emerald save writer " VERSION "\n\n");
    Con_SetColor(COLOR_WHITE);

    if (!ConfigOk())
        Fail("This file is incomplete.\nBuild it again with\nmake_save_writer.py.\n");
    SaveCore_Init();
    WaitForCart();

    Con_Print("Cart matches your dump");
    if (!IdentityOk())
    {
        Con_Putc('?');
        Con_Putc('\n');
        Fail("No. This only writes to the\ncart it was built for.\nNothing was written.\n");
    }
    PrintOk();

    refusal = ProbeChip();
    if (refusal)
        Fail(refusal);

    ReadSaveInfo(ROM_ADDR(sCfg->saveAddr), &cartInfo, FALSE);
    ReadSaveInfo(sSave, &newInfo, FALSE);
    PrintSaveInfo("On the cart:", &cartInfo);
    PrintSaveInfo("New save:", &newInfo);

    if (SaveWritten(sSave, sCfg->saveAddr))
    {
        Con_SetColor(COLOR_GREEN);
        Con_Print("\nThe cart already holds this\nsave. Nothing to do.\n");
        Finish(0);
    }
    Con_Print("Will rewrite ");
    Con_Hex(sCfg->saveAddr, 6);
    Con_Putc('-');
    Con_Hex(sCfg->saveAddr + SAVE_SIZE - 1, 6);
    Con_Putc('\n');
    Con_SetColor(COLOR_YELLOW);
    Con_Print("\nThe cart's save will be\nreplaced. Keep the GBA on.\nTo write, hold L+R, press A.\nTo cancel, turn the GBA off.\n\n");
    Con_SetColor(COLOR_WHITE);

    WaitConfirm();

    for (;;)
    {
        rc = WriteSave(sSave, sCfg->saveAddr);
        if (rc == REPRO_OK)
            break;
        PrintError(rc);
        Con_Print("Press A to try again. Or\nturn off and send this again\nlater to finish the write.\n");
        ErrorWait(rc);
    }

    Con_SetColor(COLOR_GREEN);
    Con_Print("\nDone. Turn the GBA off.\n");
    Finish(0);
}
