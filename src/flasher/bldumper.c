// GBA Link Cable Dumper, batteryless edition: the GBA side.
//
// Replaces the GBA program of FIX94's GBA Link Cable Dumper v1.6 (MIT
// license, gba/source/main.c) and speaks the same protocol, so the Wii side
// runs unchanged once make_batteryless_dumper.py has put this program into
// its boot.dol. On batteryless repro carts, which keep the save in ROM flash,
// the dumper's "backup save" and "restore save" read and write that save area.
// Game and BIOS dumps work as before. Saves of ordinary carts are not handled
// (use the original dumper for those).
//
// A restore is shown and confirmed here, on the GBA, and goes through the
// same checks and steps as the save writer (savecore.c): board probe, CFI,
// then the save area only, erased by commands sent inside it.

#include "repro_types.h"
#include "gba/io_reg.h"
#include "console.h"
#include "config.h"
#include "savecore.h"

#define VERSION "v0.1"

#define JOYCNT    (*(vu16 *)0x04000140)
#define JOY_RECV  (*(vu32 *)0x04000150)
#define JOY_TRANS (*(vu32 *)0x04000154)
#define JOY_WRITE 2 // the Wii wrote JOY_RECV
#define JOY_READ  4 // the Wii read JOY_TRANS
#define JOY_RW    6

#define ROM32(off)    (*(vu32 *)(0x08000000 + (off)))
#define ROM_ADDR(off) ((const void *)(0x08000000 + (off)))

#define GAME_AREA      0x1000000  // batteryless saves sit in the first 16 MiB
#define RECEIVE_BUFFER ((u32 *)0x02020000) // a save being restored (128 KiB)

#define GAME_CODE_BPEE 0x45455042

u32 BiosMidiKey2Freq(u32 wave, u32 key, u32 fine);

static u32 sSaveAddr;

static void WaitWrite(void)
{
    while (!(JOYCNT & JOY_WRITE))
        ;
    JOYCNT = JOY_RW;
}

static void WaitRead(void)
{
    while (!(JOYCNT & JOY_READ))
        ;
    JOYCNT = JOY_RW;
}

// Same as the original: the ROM ends where reads return the open bus
// pattern (each halfword its own address).
static s32 GameSize(void)
{
    s32 size;
    u32 j;
    vu16 *rom;

    if (ROM32(4) != 0x51AEFF24)
        return -1;
    for (size = 1 << 20; size < (1 << 25); size <<= 1)
    {
        rom = (vu16 *)(0x08000000 + size);
        for (j = 0; j < 0x1000; j++)
        {
            if (rom[j] != j)
                break;
        }
        if (j == 0x1000)
            break;
    }
    return size;
}

static bool32 SlotComplete(u32 base, u32 slot)
{
    u32 i, id, sector;
    u32 seen = 0;
    u32 counter = 0;

    for (i = 0; i < SECTORS_PER_SLOT; i++)
    {
        sector = base + (slot * SECTORS_PER_SLOT + i) * SECTOR_SIZE;
        if (ROM32(sector + 0xFF8) != SECTOR_SIGNATURE)
            return FALSE;
        id = ROM32(sector + 0xFF4) & 0xFFFF;
        if (id >= SECTORS_PER_SLOT || (seen & (1 << id)))
            return FALSE;
        seen |= 1 << id;
        if (i == 0)
            counter = ROM32(sector + 0xFFC);
        else if (ROM32(sector + 0xFFC) != counter)
            return FALSE;
    }
    return TRUE;
}

// The batteryless boot code on these carts (the cart's entry point jumps to
// it) loads the save address as "mov r0, #page; lsl r0, r0, #16" between
// selecting SRAM bank 0 and copying the save to SRAM. This finds the save
// area even when an interrupted write left no valid save in it.
static const u32 sBootCode[8] =
{
    0xE3A00409, // mov r0, #0x09000000
    0xE3A01000, // mov r1, #0
    0xE5C01000, // strb r1, [r0]
    0xE3A00000, // mov r0, #page
    0xE1A00800, // lsl r0, r0, #16
    0xE3A01302, // mov r1, #0x08000000
    0xE1800001, // orr r0, r0, r1
    0xE3A0140E, // mov r1, #0x0E000000
};

static bool32 SaveFromBootCode(u32 *addr)
{
    u32 entry = ROM32(0);
    u32 code, off, i, w;

    if ((entry & 0xFF800000) != 0xEA000000) // forward branch
        return FALSE;
    code = 8 + ((entry & 0x007FFFFF) << 2);
    if (code + 0x1000 > GAME_AREA)
        return FALSE;
    for (off = code; off < code + 0x1000; off += 4)
    {
        for (i = 0; i < 8; i++)
        {
            w = ROM32(off + i * 4);
            if (i == 3 ? (w & 0xFFFFFF00) != sBootCode[3] : w != sBootCode[i])
                break;
        }
        if (i == 8)
        {
            *addr = (ROM32(off + 12) & 0xFF) << 16;
            return *addr != 0 && *addr + SAVE_SIZE <= GAME_AREA;
        }
    }
    return FALSE;
}

// Otherwise the save is found by its contents: a 128 KiB area, 64 KiB
// aligned, holding at least one complete save slot. There must be one only.
static bool32 FindSave(s32 gameSize, bool32 *valid)
{
    u32 limit = (u32)gameSize < GAME_AREA ? (u32)gameSize : GAME_AREA;
    u32 base, addr;
    u32 found = 0;

    if (SaveFromBootCode(&addr))
    {
        sSaveAddr = addr;
        *valid = SlotComplete(addr, 0) || SlotComplete(addr, 1);
        return TRUE;
    }
    for (base = 0; base + SAVE_SIZE <= limit; base += 0x10000)
    {
        if (SlotComplete(base, 0) || SlotComplete(base, 1))
        {
            sSaveAddr = base;
            found++;
        }
    }
    *valid = TRUE;
    return found == 1;
}

static bool32 Confirm(void)
{
    u16 keys;
    u16 prev = 0;

    while (ReadKeys())
        WaitVBlank();
    for (;;)
    {
        WaitVBlank();
        keys = ReadKeys();
        if ((keys & (KEY_L | KEY_R)) == (KEY_L | KEY_R) && (keys & KEY_A) && !(prev & KEY_A))
            return TRUE;
        if ((keys & KEY_B) && !(prev & KEY_B))
            return FALSE;
        prev = keys;
    }
}

static bool32 RetryOrStop(void)
{
    u16 keys;

    while (ReadKeys())
        WaitVBlank();
    for (;;)
    {
        WaitVBlank();
        keys = ReadKeys();
        if (keys & KEY_A)
            return TRUE;
        if (keys & KEY_B)
            return FALSE;
    }
}

static void Say(u32 color, const char *msg)
{
    Con_SetColor(color);
    Con_Print(msg);
    Con_SetColor(COLOR_WHITE);
}

static void RestoreSave(void)
{
    struct SaveInfo cartInfo, newInfo;
    const u8 *save = (const u8 *)RECEIVE_BUFFER;
    bool32 emerald = ROM32(0xAC) == GAME_CODE_BPEE;
    const char *refusal;
    u16 rc;

    ReadSaveInfo(save, &newInfo, emerald);
    if (!newInfo.valid)
    {
        Say(COLOR_RED, emerald ? "The file is not a valid\nEmerald save. Nothing was\nwritten.\n"
                               : "The file is not a valid\nsave. Nothing was written.\n");
        return;
    }
    ReadSaveInfo(ROM_ADDR(sSaveAddr), &cartInfo, emerald);
    PrintSaveInfo("On the cart:", &cartInfo);
    PrintSaveInfo("From the SD card:", &newInfo);

    refusal = ProbeChip();
    if (refusal)
    {
        Say(COLOR_RED, refusal);
        return;
    }
    if (SaveWritten(save, sSaveAddr))
    {
        Say(COLOR_GREEN, "The cart already holds this\nsave. Nothing to do.\n");
        return;
    }
    Con_Print("Will rewrite ");
    Con_Hex(sSaveAddr, 6);
    Con_Putc('-');
    Con_Hex(sSaveAddr + SAVE_SIZE - 1, 6);
    Con_Putc('\n');
    Say(COLOR_YELLOW, "The cart's save will be\nreplaced. Keep the GBA on.\nTo write, hold L+R, press A.\nTo cancel, press B.\n");
    if (!Confirm())
    {
        Say(COLOR_YELLOW, "Cancelled. Nothing was\nwritten.\n");
        return;
    }

    for (;;)
    {
        rc = WriteSave(save, sSaveAddr);
        if (rc == REPRO_OK)
            break;
        PrintError(rc);
        Con_Print("A: try again. B: stop (the\nsave may be incomplete).\n");
        if (!RetryOrStop())
        {
            Say(COLOR_RED, "Stopped. Restore again\nbefore playing.\n");
            return;
        }
    }
    Say(COLOR_GREEN, "Save written.\n");
}

static void Session(void)
{
    s32 gameSize = GameSize();
    u32 saveSize = 0;
    u32 choice, i;
    bool32 valid = FALSE;

    if (gameSize > 0 && FindSave(gameSize, &valid))
        saveSize = SAVE_SIZE;

    // Both sizes are ready before the handshake: the Wii reads each one
    // right after acknowledging the previous one.
    JOY_TRANS = gameSize;
    WaitWrite();
    JOY_TRANS = saveSize;
    WaitWrite();
    if (gameSize == -1)
    {
        Con_Print("No cart found.\n");
        return;
    }
    for (i = 0; i < 0xC0; i += 4)
    {
        JOY_TRANS = ROM32(i);
        WaitRead();
    }
    JOY_TRANS = 0;

    // The Wii now waits for a button press, so there is time to print.
    Con_Print("\nCart ");
    for (i = 0; i < 4; i++)
        Con_Putc(((const vu8 *)0x080000AC)[i]);
    Con_Print(", ");
    Con_Dec(gameSize >> 20);
    Con_Print(" MB\n");
    if (saveSize)
    {
        Con_Print("Batteryless save at ");
        Con_Hex(sSaveAddr, 6);
        Con_Putc('\n');
        if (!valid)
            Say(COLOR_YELLOW, "It holds no valid save: an\nearlier write may have been\ncut off. Restore a save.\n");
    }
    else
    {
        Con_Print("No batteryless save found.\nFor saves on normal carts,\nuse the original dumper.\n");
    }

    WaitWrite();
    choice = JOY_RECV;
    if (choice == 1)
    {
        Con_Print("Dumping the game\n");
        for (i = 0; i < (u32)gameSize; i += 4)
        {
            JOY_TRANS = ROM32(i);
            if ((i & 0xFFFF) == 0)
            {
                Con_ClearLine();
                Con_Bar(i >> 16, (u32)gameSize >> 16);
            }
            WaitRead();
        }
        Con_ClearLine();
        Con_Print("Game dumped.\n");
    }
    else if (choice == 2 && saveSize)
    {
        struct SaveInfo info;

        ReadSaveInfo(ROM_ADDR(sSaveAddr), &info, ROM32(0xAC) == GAME_CODE_BPEE);
        PrintSaveInfo("Backing up:", &info);
        JOY_TRANS = saveSize;
        WaitWrite();
        for (i = 0; i < saveSize; i += 4)
        {
            JOY_TRANS = ROM32(sSaveAddr + i);
            if ((i & 0xFFF) == 0)
            {
                Con_ClearLine();
                Con_Bar(i, saveSize);
            }
            WaitRead();
        }
        Con_ClearLine();
        Say(COLOR_GREEN, "Save sent.\n");
    }
    else if (choice == 3 && saveSize)
    {
        JOY_TRANS = saveSize;
        Con_Print("Receiving a save\n");
        for (i = 0; i < saveSize; i += 4)
        {
            WaitWrite();
            RECEIVE_BUFFER[i / 4] = JOY_RECV;
        }
        RestoreSave();
        JOY_TRANS = 0;
        WaitWrite();
        Con_Print("(The TV says \"restored\" in\nany case: this screen tells.)\n");
    }
    else if (choice == 4 && saveSize)
    {
        // Let the Wii see the size once, as it expects, then report done.
        JOY_TRANS = saveSize;
        WaitRead();
        Say(COLOR_YELLOW, "Clearing is not supported\nhere. Nothing was changed.\n");
        JOY_TRANS = 0;
        WaitWrite();
    }
}

static u32 BiosWord(u32 i)
{
    u32 a = BiosMidiKey2Freq(i - 4, 180 - 12, 0) * 2;
    u32 b = BiosMidiKey2Freq(i - 3, 180 - 12, 0) * 2;
    u32 c = BiosMidiKey2Freq(i - 2, 180 - 12, 0) * 2;
    u32 d = BiosMidiKey2Freq(i - 1, 180 - 12, 0) * 2;

    return (a >> 24 << 24) | (d >> 24 << 16) | (c >> 24 << 8) | (b >> 24);
}

// The original's BIOS dump (by Dark Fader). The next word is worked out
// while the Wii reads the current one.
static void DumpBios(void)
{
    u32 i;
    u32 next = BiosWord(0);

    Con_Print("Dumping the BIOS\n");
    for (i = 0; i < 0x4000; i += 4)
    {
        JOY_TRANS = next;
        if (i + 4 < 0x4000)
            next = BiosWord(i + 4);
        WaitRead();
    }
    Con_Print("BIOS dumped.\n");
}

void AgbMain(void)
{
    u16 flags;

    REG_WAITCNT = 0x4014; // ROM 3/1 wait states, SRAM 4, as Emerald uses
    Con_Init();
    Say(COLOR_YELLOW, "GBA Link Cable Dumper\nbatteryless edition " VERSION "\n\n");
    Con_Print("Choose what to do on the TV.\nA save restore is confirmed\nhere, on the GBA.\n");
    SaveCore_Init();

    JOY_TRANS = 0;
    JOYCNT = JOY_RW; // clear anything left from the transfer
    for (;;)
    {
        flags = JOYCNT;
        if (flags & JOY_READ)
        {
            JOYCNT = JOY_RW;
            Session();
            JOY_TRANS = 0;
        }
        else if (flags & JOY_WRITE)
        {
            JOYCNT = JOY_RW;
            if (JOY_RECV == 5)
                DumpBios();
            JOY_TRANS = 0;
        }
    }
}
