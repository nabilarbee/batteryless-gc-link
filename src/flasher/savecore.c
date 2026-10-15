// Save area writing shared by the save writer and the batteryless dumper.
// The chip and SRAM steps are those of the flasher (main.c), which worked on
// a real cart; they are repeated here so that the shipped flasher stays as
// tested.

#include "repro_types.h"
#include "repro_hw.h"
#include "repro_cart.h"
#include "gba/io_reg.h"
#include "console.h"
#include "cfi.h"
#include "config.h"
#include "savecore.h"

#define ROM8(off)     (*(vu8 *)(0x08000000 + (off)))
#define ROM_ADDR(off) ((const void *)(0x08000000 + (off)))

#define PIECE_MAX 0x10000 // one SRAM bank of data per program pass

u32 HashWords(const void *src, u32 bytes, u32 h);

// HashWords is ARM code in IWRAM, out of BL range: call it through a pointer.
u32 (*gHashWords)(const void *src, u32 bytes, u32 h) = HashWords;

struct ReproCart gCart;
struct CfiInfo gCfi;
u32 gBlock;
u32 gFFHash;
u32 gErrorAddr;
bool8 gWriteStarted;

static u32 sProgDone;
static const u8 *sSave;
static u32 sSaveAddr;

static const char *const sTypeNames[] =
{
    [REPRO_CART_UNKNOWN]        = "unknown",
    [REPRO_CART_INTEL_BUFFERED] = "Intel, buffered",
    [REPRO_CART_INTEL_WORD]     = "Intel, word",
    [REPRO_CART_AMD_SWAPPED]    = "AMD x16, D0/D1 swap",
    [REPRO_CART_AMD_DUAL]       = "AMD, two x8 chips",
    [REPRO_CART_AMD_X16]        = "AMD x16 (no support)",
};

// Bytes covered by each sector's checksum in Emerald, by sector id.
static const u16 sEmeraldSizes[SECTORS_PER_SLOT] =
{
    0xF2C, 0xF80, 0xF80, 0xF80, 0xF08, 0xF80, 0xF80, 0xF80, 0xF80, 0xF80, 0xF80, 0xF80, 0xF80, 0x7D0,
};

// ---------------------------------------------------------------- system

void WaitVBlank(void)
{
    while (REG_VCOUNT >= 160)
        ;
    while (REG_VCOUNT < 160)
        ;
}

void WaitFrames(u32 frames)
{
    while (frames--)
        WaitVBlank();
}

u16 ReadKeys(void)
{
    return ~REG_KEYINPUT & 0x3FF;
}

void ErrorWait(u32 code)
{
    while (ReadKeys())
        WaitVBlank();
    while (!(ReadKeys() & KEY_A))
        WaitVBlank();
}

void PrintOk(void)
{
    Con_SetColor(COLOR_GREEN);
    Con_Print(" ok\n");
    Con_SetColor(COLOR_WHITE);
}

void PrintDecPadded(u32 value, u32 digits)
{
    static const u32 powers[] = {10000, 1000, 100, 10, 1};
    u32 i, digit;

    for (i = 5 - digits; i < 5; i++)
    {
        digit = 0;
        while (value >= powers[i])
        {
            value -= powers[i];
            digit++;
        }
        Con_Putc('0' + digit);
    }
}

// ---------------------------------------------------------------- hashing

u32 ChunkHash(u32 off)
{
    return gHashWords(ROM_ADDR(off), HASH_CHUNK, HASH_BASIS);
}

void SaveCore_Init(void)
{
    static const u32 ones[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    u32 h = HASH_BASIS;
    u32 i;

    for (i = 0; i < HASH_CHUNK; i += sizeof(ones))
        h = gHashWords(ones, sizeof(ones), h);
    gFFHash = h;
}

bool32 IsErased(u32 addr, u32 len)
{
    u32 i;

    for (i = 0; i < len; i += HASH_CHUNK)
    {
        if (ChunkHash(addr + i) != gFFHash)
            return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------- chip

// Raw answers to the probe's own ID queries, for chips it did not recognise.
static void ShowChipIds(void)
{
    u16 ime;
    u16 header1 = ROM_READ16(2);
    u16 id0, id1;

    IRQ_DISABLE(ime);
    ROM_WRITE16(0, 0xFF);
    ROM_WRITE16(0, 0x50);
    ROM_WRITE16(0, 0x90);
    id0 = ROM_READ16(0);
    id1 = ROM_READ16(2);
    ROM_WRITE16(0, 0xFF);
    Con_Print("Intel ID ");
    Con_Hex(id0, 4);
    Con_Putc(' ');
    Con_Hex(id1, 4);
    Con_Putc('\n');
    if (id1 == header1)
    {
        ROM_WRITE16(0, 0xF0);
        ROM_WRITE16(0xAAA, 0xA9);
        ROM_WRITE16(0x554, 0x56);
        ROM_WRITE16(0xAAA, 0x90);
        id0 = ROM_READ16(0);
        id1 = ROM_READ16(2);
        ROM_WRITE16(0, 0xF0);
        Con_Print("AMD ID ");
        Con_Hex(id0, 4);
        Con_Putc(' ');
        Con_Hex(id1, 4);
        Con_Putc('\n');
    }
    ROM_WRITE16(0, 0xF0F0);
    ROM_WRITE16(0, 0xFFFF);
    IRQ_RESTORE(ime);
}

const char *ProbeChip(void)
{
    const struct ReproCart *probed = ReproCart_Probe();
    u32 i;

    for (i = 0; i < sizeof(gCart); i++)
        ((u8 *)&gCart)[i] = ((const u8 *)probed)[i];
    Con_Print("Chip: ");
    Con_Print(sTypeNames[gCart.type]);
    Con_Putc('\n');
    if (!gCart.supported)
    {
        if (gCart.type == REPRO_CART_UNKNOWN)
            ShowChipIds();
        return "This board type is not\nsupported. Nothing was\nwritten.\n";
    }
    if (!Cfi_Read(&gCart, &gCfi))
    {
        Con_Print("CFI: no answer (");
        Con_Hex(gCfi.raw[0], 2);
        Con_Hex(gCfi.raw[1], 2);
        Con_Hex(gCfi.raw[2], 2);
        Con_Print(")\n");
        return "Can't read the chip's block\nlayout. Nothing was written.\n";
    }
    gBlock = gCfi.blockSize;
    Con_Print("Blocks: ");
    Con_Dec(gBlock / 1024);
    Con_Print(" KB, chip ");
    Con_Dec(gCfi.busSize >> 20);
    Con_Print(" MB\n");
    if (gBlock < MIN_BLOCK || gBlock > MAX_BLOCK || (gBlock & (gBlock - 1)) || gCfi.busSize < 0x1000000)
        return "This block layout is not\nsupported. Nothing was\nwritten.\n";
    return NULL;
}

// ---------------------------------------------------------------- save area
//
// Only the save area is ever erased, and only by erase commands sent to
// addresses inside it. The cart's own save routine sends its erase commands
// there too, every time the game saves, so whatever shares an erase block
// with the save cannot hold anything the cart needs, whatever the real block
// size. If blocks are smaller than the chip reports, parts of the area stay
// unerased after the first pass; those are found and erased in a second pass.
// Everything is erased before anything is programmed, so blocks larger than
// reported cannot wipe data already written.

bool32 SaveWritten(const u8 *save, u32 saveAddr)
{
    u32 i;

    for (i = 0; i < SAVE_SIZE; i += HASH_CHUNK)
    {
        if (ChunkHash(saveAddr + i) != gHashWords(save + i, HASH_CHUNK, HASH_BASIS))
            return FALSE;
    }
    return TRUE;
}

static u16 FillSram(u32 addr, u32 len)
{
    u32 i;

    for (i = 0; i < len; i++)
        ReproSram_Write8(i, sSave[addr + i - sSaveAddr], gCart.bankValue);
    for (i = 0; i < len; i++)
    {
        if (ReproSram_Read8(i, gCart.bankValue) != sSave[addr + i - sSaveAddr])
        {
            gErrorAddr = addr + i;
            return REPRO_ERR_SRAM;
        }
    }
    return REPRO_OK;
}

static u16 ProgramFromSram(u32 dst, u32 len)
{
    u32 unit = gCart.programUnit;
    u32 off, i;
    u16 rc;

    for (off = 0; off < len; off += unit)
    {
        if ((off & 0xFFF) == 0)
            Con_Bar(sProgDone + off, SAVE_SIZE);
        for (i = 0; i < unit; i++)
        {
            if (ReproSram_Read8(off + i, gCart.bankValue) != 0xFF)
                break;
        }
        if (i == unit)
            continue; // already the erased state
        rc = ReproCart_ProgramUnit(&gCart, dst + off, off);
        if (rc != REPRO_OK)
        {
            gErrorAddr = dst + off;
            return rc;
        }
    }
    sProgDone += len;
    return REPRO_OK;
}

static u16 Erase(u32 addr)
{
    u16 rc;

    gErrorAddr = addr;
    rc = ReproCart_EraseBlock(&gCart, addr);
    if (rc != REPRO_OK)
        rc = ReproCart_EraseBlock(&gCart, addr);
    return rc;
}

static u16 EraseSaveArea(void)
{
    u32 step = gBlock < SAVE_SIZE ? gBlock : SAVE_SIZE;
    u32 i;
    u16 rc;

    for (i = 0; i < SAVE_SIZE; i += step)
    {
        rc = Erase(sSaveAddr + i);
        if (rc != REPRO_OK)
            return rc;
    }
    for (i = 0; i < SAVE_SIZE; i += HASH_CHUNK)
    {
        if (ChunkHash(sSaveAddr + i) != gFFHash)
        {
            rc = Erase(sSaveAddr + i);
            if (rc != REPRO_OK)
                return rc;
        }
    }
    gErrorAddr = sSaveAddr;
    return IsErased(sSaveAddr, SAVE_SIZE) ? REPRO_OK : REPRO_ERR_VERIFY;
}

static u16 WriteOnce(void)
{
    u32 p;
    u16 rc;

    Con_ClearLine();
    Con_Print("Erasing ");
    Con_Hex(sSaveAddr, 6);
    gWriteStarted = TRUE;
    rc = EraseSaveArea();
    if (rc != REPRO_OK)
        return rc;
    PrintOk();
    Con_Print("Writing ");
    sProgDone = 0;
    for (p = 0; p < SAVE_SIZE && rc == REPRO_OK; p += PIECE_MAX)
    {
        rc = FillSram(sSaveAddr + p, PIECE_MAX);
        if (rc == REPRO_OK)
            rc = ProgramFromSram(sSaveAddr + p, PIECE_MAX);
    }
    if (rc != REPRO_OK)
        return rc;
    gErrorAddr = sSaveAddr;
    if (!SaveWritten(sSave, sSaveAddr))
        return ERR_WRITE_VERIFY;
    Con_ClearLine();
    Con_Print("Writing ");
    Con_Hex(sSaveAddr, 6);
    PrintOk();
    return REPRO_OK;
}

u16 WriteSave(const u8 *save, u32 saveAddr)
{
    u16 rc;

    sSave = save;
    sSaveAddr = saveAddr;
    ReproSram_ForgetBank();
    rc = WriteOnce();
    if (rc != REPRO_OK)
    {
        Con_Putc('\n');
        rc = WriteOnce();
    }
    return rc;
}

// ---------------------------------------------------------------- save info

static u32 Get32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static u16 Get16(const u8 *p)
{
    return p[0] | (p[1] << 8);
}

static bool32 ChecksumOk(const u8 *sector, u32 id)
{
    u32 sum = 0;
    u32 i;

    for (i = 0; i < sEmeraldSizes[id]; i += 4)
        sum += Get32(sector + i);
    return (u16)((sum >> 16) + sum) == Get16(sector + 0xFF6);
}

// The slot the game loads: 14 signed sectors with ids 0-13 and one save
// count (and, for Emerald, valid checksums), the higher count winning.
void ReadSaveInfo(const u8 *save, struct SaveInfo *info, bool32 emeraldChecksums)
{
    u32 slot, i, id, seen, counter;
    const u8 *sector;
    const u8 *sb2;

    info->valid = FALSE;
    for (slot = 0; slot < 2; slot++)
    {
        seen = 0;
        counter = 0;
        sb2 = save;
        for (i = 0; i < SECTORS_PER_SLOT; i++)
        {
            sector = save + (slot * SECTORS_PER_SLOT + i) * SECTOR_SIZE;
            id = Get16(sector + 0xFF4);
            if (Get32(sector + 0xFF8) != SECTOR_SIGNATURE || id >= SECTORS_PER_SLOT || (seen & (1 << id)))
                break;
            if (emeraldChecksums && !ChecksumOk(sector, id))
                break;
            if (i == 0)
                counter = Get32(sector + 0xFFC);
            else if (Get32(sector + 0xFFC) != counter)
                break;
            seen |= 1 << id;
            if (id == 0)
                sb2 = sector;
        }
        if (i < SECTORS_PER_SLOT || (info->valid && counter <= info->counter))
            continue;
        info->valid = TRUE;
        info->counter = counter;
        for (i = 0; i < 7; i++)
            info->name[i] = sb2[i];
        info->name[7] = 0xFF;
        info->trainerId = Get16(sb2 + 0x0A);
        info->hours = Get16(sb2 + 0x0E);
        info->minutes = sb2[0x10];
    }
}

static u32 GameChar(u8 c)
{
    if (c >= 0xBB && c <= 0xD4)
        return 'A' + (c - 0xBB);
    if (c >= 0xD5 && c <= 0xEE)
        return 'a' + (c - 0xD5);
    if (c >= 0xA1 && c <= 0xAA)
        return '0' + (c - 0xA1);
    switch (c)
    {
    case 0x00: return ' ';
    case 0xAB: return '!';
    case 0xAC: return '?';
    case 0xAD: return '.';
    case 0xAE: return '-';
    case 0xB8: return ',';
    case 0xBA: return '/';
    }
    return '?';
}

void PrintSaveInfo(const char *label, const struct SaveInfo *info)
{
    u32 i;

    Con_Print(label);
    if (!info->valid)
    {
        Con_Print(" none\n");
        return;
    }
    Con_Print("\n  ");
    for (i = 0; i < 7 && info->name[i] != 0xFF; i++)
        Con_Putc(GameChar(info->name[i]));
    for (; i < 8; i++)
        Con_Putc(' ');
    Con_Print("ID ");
    PrintDecPadded(info->trainerId, 5);
    Con_Print("  ");
    Con_Dec(info->hours);
    Con_Putc(':');
    PrintDecPadded(info->minutes < 60 ? info->minutes : 0, 2);
    Con_Putc('\n');
}

// ---------------------------------------------------------------- errors

static const char *ErrorText(u16 rc)
{
    switch (rc)
    {
    case REPRO_ERR_TIMEOUT:  return "The chip did not finish.";
    case REPRO_ERR_STATUS:   return "The chip reported a failure.";
    case REPRO_ERR_VERIFY:   return "Data read back wrong.";
    case REPRO_ERR_SRAM:     return "Cart SRAM read back wrong.";
    case REPRO_ERR_LOCKED:   return "A block stayed locked.";
    case ERR_WRITE_VERIFY:   return "The save read back wrong.";
    }
    return "Unknown error.";
}

void PrintError(u16 rc)
{
    Con_Putc('\n');
    Con_SetColor(COLOR_RED);
    Con_Print("Error ");
    Con_Hex(rc, 4);
    Con_Print(" at ");
    Con_Hex(gErrorAddr, 6);
    Con_Putc('\n');
    Con_Print(ErrorText(rc));
    Con_Putc('\n');
    Con_SetColor(COLOR_WHITE);
}
