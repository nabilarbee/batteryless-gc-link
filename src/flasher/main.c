// Emerald link flasher.
//
// A GBA multiboot program, sent over the GameCube link cable with a batteryless
// repro Pokemon Emerald cart inserted. It rewrites the parts of the cart's ROM
// flash that hold Emerald's built-in Colosseum link program (with the repro
// build) and Emerald's link code (with the hook for XD). Everything specific
// to the cart comes from the data block that make_flasher.py appends from the
// user's own dump (config.h).
//
// Safety design:
//  - Nothing is written until the cart's ROM matches that dump (hash over
//    most of the ROM) and the user confirms with L+R+A.
//  - The erase block size comes from the chip's CFI table and is then checked
//    by experiment in an area that is unused (0xFF) in the dump, before any
//    game data is touched.
//  - Each block is first copied, with the new bytes in place, to a spare
//    unused block and checked there. Only then is the game block erased and
//    rewritten from that copy. Wherever power is lost, running the flasher
//    again finishes the job from what is on the cart.
//  - The save area and the cart's own batteryless code are never written.
//  - Flash commands are the ones the repro save code uses (repro_cart.c),
//    with the cart's banked SRAM as the source buffer, as in a save.

#include "repro_types.h"
#include "repro_hw.h"
#include "repro_cart.h"
#include "gba/io_reg.h"
#include "console.h"
#include "cfi.h"
#include "config.h"

#define VERSION "v0.4"

#define ROM8(off)     (*(vu8 *)(0x08000000 + (off)))
#define ROM_ADDR(off) ((const void *)(0x08000000 + (off)))

#define KEY_A 0x0001
#define KEY_R 0x0100
#define KEY_L 0x0200

#define MIN_BLOCK 0x1000
#define MAX_BLOCK 0x40000
#define PIECE_MAX 0x10000 // one SRAM bank of data per program pass

// Errors of the flasher itself; the chip layer uses 0x81xx (repro_cart.h).
#define ERR_BLOCK_SMALLER 0x8201
#define ERR_BLOCK_LARGER  0x8202
#define ERR_STAGE_VERIFY  0x8203
#define ERR_WRITE_VERIFY  0x8204
#define ERR_UNKNOWN_STATE 0x8205
#define ERR_MARK_VERIFY   0x8206
#define ERR_SIZE_TEST     0x8207

enum
{
    BLOCK_TARGET,
    BLOCK_SOURCE,   // a state the block may be in before it is written
    BLOCK_OTHER,
};

#define TABLE_TARGET 0

extern u8 __image_end[];
u32 HashWords(const void *src, u32 bytes, u32 h);

// HashWords is ARM code in IWRAM, out of BL range: call it through a pointer.
u32 (*gHashWords)(const void *src, u32 bytes, u32 h) = HashWords;

static const struct FlasherConfig *sCfg;
static struct ReproCart sCart;
static struct CfiInfo sCfi;
static u32 sBlock;
static u32 sPiece;
static u32 sFFHash;
static u32 sErrorAddr;
static u32 sProgDone;
static u16 sTestError;

static const char *const sTypeNames[] =
{
    [REPRO_CART_UNKNOWN]        = "unknown",
    [REPRO_CART_INTEL_BUFFERED] = "Intel, buffered",
    [REPRO_CART_INTEL_WORD]     = "Intel, word",
    [REPRO_CART_AMD_SWAPPED]    = "AMD x16, D0/D1 swap",
    [REPRO_CART_AMD_DUAL]       = "AMD, two x8 chips",
    [REPRO_CART_AMD_X16]        = "AMD x16 (no support)",
};

// ---------------------------------------------------------------- system

void WaitVBlank(void)
{
    while (REG_VCOUNT >= 160)
        ;
    while (REG_VCOUNT < 160)
        ;
}

static void WaitFrames(u32 frames)
{
    while (frames--)
        WaitVBlank();
}

u16 ReadKeys(void)
{
    return ~REG_KEYINPUT & 0x3FF;
}

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

static void PrintOk(void)
{
    Con_SetColor(COLOR_GREEN);
    Con_Print(" ok\n");
    Con_SetColor(COLOR_WHITE);
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

void ErrorWait(u32 code)
{
    while (ReadKeys())
        WaitVBlank();
    while (!(ReadKeys() & KEY_A))
        WaitVBlank();
}

// ---------------------------------------------------------------- data

static bool32 ConfigOk(void)
{
    u32 i;

    sCfg = (const struct FlasherConfig *)(((u32)__image_end + 31) & ~31);
    if (sCfg->magic != FLASHER_MAGIC || sCfg->version != FLASHER_VERSION)
        return FALSE;
    if (sCfg->totalSize < sizeof(struct FlasherConfig) || (sCfg->totalSize & 31)
     || (u32)sCfg + sCfg->totalSize > 0x02040000)
        return FALSE;
    if (gHashWords((const u8 *)sCfg + 0x20, sCfg->totalSize - 0x20, HASH_BASIS) != sCfg->bodyHash)
        return FALSE;
    if (sCfg->numIdRanges > MAX_ID_RANGES || sCfg->numSpans == 0 || sCfg->numSpans > MAX_SPANS)
        return FALSE;
    for (i = 0; i < sCfg->numSpans; i++)
    {
        const struct FlasherSpan *sp = &sCfg->spans[i];
        u32 chunks = (sp->hashEnd - sp->hashStart) / HASH_CHUNK;

        if (sp->numSources == 0 || sp->numSources > MAX_SOURCES
         || sp->hashStart > sp->overlayStart || sp->overlayStart >= sp->overlayEnd || sp->overlayEnd > sp->hashEnd
         || (sp->hashStart & (MAX_BLOCK - 1)) || (sp->hashEnd & (MAX_BLOCK - 1))
         || sp->tablesOffset + (1 + sp->numSources) * chunks * 4 > sCfg->totalSize
         || sp->bytesOffset + (sp->overlayEnd - sp->overlayStart) > sCfg->totalSize)
            return FALSE;
    }
    return TRUE;
}

static u32 ChunkHash(u32 off)
{
    return gHashWords(ROM_ADDR(off), HASH_CHUNK, HASH_BASIS);
}

static u32 FFHash(void)
{
    static const u32 ones[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    u32 h = HASH_BASIS;
    u32 i;

    for (i = 0; i < HASH_CHUNK; i += sizeof(ones))
        h = gHashWords(ones, sizeof(ones), h);
    return h;
}

static bool32 IsErased(u32 addr, u32 len)
{
    u32 i;

    for (i = 0; i < len; i += HASH_CHUNK)
    {
        if (ChunkHash(addr + i) != sFFHash)
            return FALSE;
    }
    return TRUE;
}

// The span whose hash area holds game block x.
static const struct FlasherSpan *SpanOf(u32 x)
{
    u32 i;

    for (i = 0; i < sCfg->numSpans; i++)
    {
        if (x >= sCfg->spans[i].hashStart && x < sCfg->spans[i].hashEnd)
            return &sCfg->spans[i];
    }
    return NULL;
}

// Whether the block at 'addr' holds what hash table 'table' of the span of
// game block x says x should hold (table 0: written, 1..: the sources).
static bool32 BlockMatches(u32 addr, u32 x, u32 table)
{
    const struct FlasherSpan *sp = SpanOf(x);
    const u32 *hashes;
    u32 chunks, first, i;

    if (sp == NULL)
        return FALSE;
    chunks = (sp->hashEnd - sp->hashStart) / HASH_CHUNK;
    hashes = (const u32 *)((const u8 *)sCfg + sp->tablesOffset) + table * chunks;
    first = (x - sp->hashStart) / HASH_CHUNK;
    for (i = 0; i < sBlock; i += HASH_CHUNK)
    {
        if (ChunkHash(addr + i) != hashes[first + i / HASH_CHUNK])
            return FALSE;
    }
    return TRUE;
}

static u32 BlockState(u32 x)
{
    const struct FlasherSpan *sp = SpanOf(x);
    u32 t;

    if (BlockMatches(x, x, TABLE_TARGET))
        return BLOCK_TARGET;
    for (t = 1; sp != NULL && t <= sp->numSources; t++)
    {
        if (BlockMatches(x, x, t))
            return BLOCK_SOURCE;
    }
    return BLOCK_OTHER;
}

static u32 SpanFirst(const struct FlasherSpan *sp)
{
    return sp->overlayStart & ~(sBlock - 1);
}

static u32 SpanEnd(const struct FlasherSpan *sp)
{
    return (sp->overlayEnd + sBlock - 1) & ~(sBlock - 1);
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

// ---------------------------------------------------------------- flash steps

static u8 TargetByte(u32 addr)
{
    u32 i;

    for (i = 0; i < sCfg->numSpans; i++)
    {
        const struct FlasherSpan *sp = &sCfg->spans[i];

        if (addr >= sp->overlayStart && addr < sp->overlayEnd)
            return ((const u8 *)sCfg + sp->bytesOffset)[addr - sp->overlayStart];
    }
    return ROM8(addr);
}

// Fills SRAM bank 0 from 'len' bytes of ROM at 'src' (with the new program
// bytes in place when 'withNewBytes' is set) and reads it back.
static u16 FillSram(u32 src, u32 len, bool32 withNewBytes)
{
    u32 i;
    u8 value;

    for (i = 0; i < len; i++)
    {
        value = withNewBytes ? TargetByte(src + i) : ROM8(src + i);
        ReproSram_Write8(i, value, sCart.bankValue);
    }
    for (i = 0; i < len; i++)
    {
        value = withNewBytes ? TargetByte(src + i) : ROM8(src + i);
        if (ReproSram_Read8(i, sCart.bankValue) != value)
        {
            sErrorAddr = src + i;
            return REPRO_ERR_SRAM;
        }
    }
    return REPRO_OK;
}

static u16 ProgramFromSram(u32 dst, u32 len)
{
    u32 unit = sCart.programUnit;
    u32 off, i;
    u16 rc;

    for (off = 0; off < len; off += unit)
    {
        if ((off & 0xFFF) == 0)
            Con_Bar(sProgDone + off, sBlock);
        for (i = 0; i < unit; i++)
        {
            if (ReproSram_Read8(off + i, sCart.bankValue) != 0xFF)
                break;
        }
        if (i == unit)
            continue; // already the erased state
        rc = ReproCart_ProgramUnit(&sCart, dst + off, off);
        if (rc != REPRO_OK)
        {
            sErrorAddr = dst + off;
            return rc;
        }
    }
    sProgDone += len;
    return REPRO_OK;
}

static u16 EraseChecked(u32 addr)
{
    u16 rc;

    sErrorAddr = addr;
    rc = ReproCart_EraseBlock(&sCart, addr);
    if (rc != REPRO_OK)
        rc = ReproCart_EraseBlock(&sCart, addr);
    if (rc == REPRO_OK && !IsErased(addr, sBlock))
        rc = REPRO_ERR_VERIFY;
    return rc;
}

static void StepStart(u32 addr, const char *what)
{
    Con_ClearLine();
    Con_Hex(addr, 6);
    Con_Print(what);
    sProgDone = 0;
    Con_Bar(0, sBlock);
}

static void StepDone(u32 addr, const char *what)
{
    Con_ClearLine();
    Con_Hex(addr, 6);
    Con_Print(what);
    PrintOk();
}

static u8 MarkByte(u32 i)
{
    return 0xA5 ^ i;
}

static bool32 MarkPresent(u32 addr)
{
    u32 i;

    for (i = 0; i < sCart.programUnit; i++)
    {
        if (ROM8(addr + i) != MarkByte(i))
            return FALSE;
    }
    return TRUE;
}

// Checks that one erase command clears exactly sBlock bytes, using the test
// area (unused in the dump): a marker goes in the first and last program unit
// of the test block and next to it on each side, then the test block is erased.
static u16 EraseSizeTest(void)
{
    u32 t = sCfg->testAddr;
    u32 unit = sCart.programUnit;
    u32 marks[4];
    u32 i;
    u16 rc;

    marks[0] = t - unit;
    marks[1] = t;
    marks[2] = t + sBlock - unit;
    marks[3] = t + sBlock;

    rc = EraseChecked(t - sBlock);
    if (rc == REPRO_OK)
        rc = EraseChecked(t);
    if (rc == REPRO_OK)
        rc = EraseChecked(t + sBlock);
    if (rc != REPRO_OK)
        return rc;

    for (i = 0; i < unit; i++)
        ReproSram_Write8(i, MarkByte(i), sCart.bankValue);
    for (i = 0; i < 4; i++)
    {
        // A marker that cannot be programmed can mean the blocks are smaller
        // than reported (the marker's block was not erased or unlocked).
        // Retrying cannot fix that, so stop while only the spare area has
        // been written.
        sErrorAddr = marks[i];
        rc = ReproCart_ProgramUnit(&sCart, marks[i], 0);
        if (rc == REPRO_OK && !MarkPresent(marks[i]))
            rc = ERR_MARK_VERIFY;
        if (rc != REPRO_OK)
        {
            sTestError = rc;
            return ERR_SIZE_TEST;
        }
    }

    sErrorAddr = t;
    rc = ReproCart_EraseBlock(&sCart, t);
    if (rc != REPRO_OK)
        rc = ReproCart_EraseBlock(&sCart, t);
    if (rc != REPRO_OK)
        return rc;
    if (!IsErased(t, sBlock))
        return ERR_BLOCK_SMALLER;
    if (!MarkPresent(marks[0]) || !MarkPresent(marks[3]))
        return ERR_BLOCK_LARGER;
    return REPRO_OK;
}

// Copies game block x to the staging block, with the new bytes in place.
static u16 Stage(u32 x)
{
    u32 s = sCfg->stagingAddr;
    u32 p;
    u16 rc;

    StepStart(x, " copy  ");
    rc = EraseChecked(s);
    for (p = 0; p < sBlock && rc == REPRO_OK; p += sPiece)
    {
        rc = FillSram(x + p, sPiece, TRUE);
        if (rc == REPRO_OK)
            rc = ProgramFromSram(s + p, sPiece);
    }
    if (rc != REPRO_OK)
        return rc;
    sErrorAddr = s;
    if (!BlockMatches(s, x, TABLE_TARGET))
        return ERR_STAGE_VERIFY;
    StepDone(x, " copy  ");
    return REPRO_OK;
}

// Rewrites game block x from the checked copy in the staging block.
static u16 Rewrite(u32 x)
{
    u32 s = sCfg->stagingAddr;
    u32 p;
    u16 rc;

    sErrorAddr = s;
    if (!BlockMatches(s, x, TABLE_TARGET))
        return ERR_STAGE_VERIFY;
    StepStart(x, " write ");
    rc = EraseChecked(x);
    for (p = 0; p < sBlock && rc == REPRO_OK; p += sPiece)
    {
        rc = FillSram(s + p, sPiece, FALSE);
        if (rc == REPRO_OK)
            rc = ProgramFromSram(x + p, sPiece);
    }
    if (rc != REPRO_OK)
        return rc;
    sErrorAddr = x;
    if (!BlockMatches(x, x, TABLE_TARGET))
        return ERR_WRITE_VERIFY;
    StepDone(x, " write ");
    return REPRO_OK;
}

// Leaves the spare area erased again, as it is in the dump.
static u16 Cleanup(void)
{
    u32 blocks[4];
    u32 i;
    u16 rc;

    blocks[0] = sCfg->stagingAddr;
    blocks[1] = sCfg->testAddr - sBlock;
    blocks[2] = sCfg->testAddr;
    blocks[3] = sCfg->testAddr + sBlock;
    for (i = 0; i < 4; i++)
    {
        if (!IsErased(blocks[i], sBlock))
        {
            rc = EraseChecked(blocks[i]);
            if (rc != REPRO_OK)
                return rc;
        }
    }
    return REPRO_OK;
}

static bool32 SpareClean(void)
{
    return IsErased(sCfg->stagingAddr, sBlock) && IsErased(sCfg->testAddr - sBlock, 3 * sBlock);
}

static u16 WriteBlock(u32 x)
{
    u16 rc;

    sErrorAddr = x;
    if (BlockState(x) == BLOCK_TARGET)
    {
        StepDone(x, " already new");
        return REPRO_OK;
    }
    if (!BlockMatches(sCfg->stagingAddr, x, TABLE_TARGET))
    {
        // A block that is in none of the expected states can only come from
        // an interrupted write, and then the staging block holds its copy.
        // Without that copy there is nothing safe to do.
        if (BlockState(x) != BLOCK_SOURCE)
            return ERR_UNKNOWN_STATE;
        rc = Stage(x);
        if (rc != REPRO_OK)
            rc = Stage(x);
        if (rc != REPRO_OK)
            return rc;
    }
    rc = Rewrite(x);
    if (rc != REPRO_OK)
        rc = Rewrite(x);
    return rc;
}

static u16 WriteAll(void)
{
    u32 i, x;
    u16 rc;

    ReproSram_ForgetBank();

    Con_Print("Erase test in spare area");
    rc = EraseSizeTest();
    if (rc != REPRO_OK)
        return rc;
    PrintOk();

    for (i = 0; i < sCfg->numSpans; i++)
    {
        const struct FlasherSpan *sp = &sCfg->spans[i];

        for (x = SpanFirst(sp); x < SpanEnd(sp); x += sBlock)
        {
            rc = WriteBlock(x);
            if (rc != REPRO_OK)
                return rc;
        }
    }

    Con_Print("Erase spare area again");
    rc = Cleanup();
    if (rc != REPRO_OK)
        return rc;
    PrintOk();
    return REPRO_OK;
}

// Counts the blocks of all spans in each state.
static void CountBlocks(u32 *total, u32 *target, u32 *other)
{
    u32 i, x, state;

    *total = *target = *other = 0;
    for (i = 0; i < sCfg->numSpans; i++)
    {
        const struct FlasherSpan *sp = &sCfg->spans[i];

        for (x = SpanFirst(sp); x < SpanEnd(sp); x += sBlock)
        {
            state = BlockState(x);
            (*total)++;
            if (state == BLOCK_TARGET)
                (*target)++;
            else if (state == BLOCK_OTHER)
                (*other)++;
        }
    }
}

static const char *ErrorText(u16 rc)
{
    switch (rc)
    {
    case REPRO_ERR_TIMEOUT:  return "The chip did not finish.";
    case REPRO_ERR_STATUS:   return "The chip reported a failure.";
    case REPRO_ERR_VERIFY:   return "Data read back wrong.";
    case REPRO_ERR_SRAM:     return "Cart SRAM read back wrong.";
    case REPRO_ERR_LOCKED:   return "A block stayed locked.";
    case ERR_BLOCK_SMALLER:  return "Erase blocks are smaller\nthan the chip reports.";
    case ERR_BLOCK_LARGER:   return "Erase blocks are larger\nthan the chip reports.";
    case ERR_STAGE_VERIFY:   return "The spare copy is wrong.";
    case ERR_WRITE_VERIFY:   return "The rewritten block is wrong.";
    case ERR_UNKNOWN_STATE:  return "A block holds unexpected data.";
    case ERR_MARK_VERIFY:    return "Test marker read back wrong.";
    case ERR_SIZE_TEST:      return "The erase test failed.";
    }
    return "Unknown error.";
}

static bool32 IsFatal(u16 rc)
{
    return rc == ERR_BLOCK_SMALLER || rc == ERR_BLOCK_LARGER || rc == ERR_SIZE_TEST || rc == ERR_UNKNOWN_STATE;
}

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

void AgbMain(void)
{
    const struct ReproCart *probed;
    u32 x, i;
    u32 newBlocks, total, other;
    u16 rc;

    REG_WAITCNT = 0x4014; // the same as Emerald: ROM 3/1 wait states, SRAM 4
    Con_Init();
    Con_SetColor(COLOR_YELLOW);
    Con_Print("Emerald link flasher " VERSION "\n\n");
    Con_SetColor(COLOR_WHITE);

    if (!ConfigOk())
        Fail("This file is incomplete.\nBuild it again with\nmake_flasher.py.\n");
    sFFHash = FFHash();
    WaitForCart();

    Con_Print("Cart matches your dump");
    if (!IdentityOk())
    {
        Con_Putc('?');
        Con_Putc('\n');
        Fail("No. This flasher only writes\nthe cart it was built for.\nNothing was written.\n");
    }
    PrintOk();

    probed = ReproCart_Probe();
    for (i = 0; i < sizeof(sCart); i++)
        ((u8 *)&sCart)[i] = ((const u8 *)probed)[i];
    Con_Print("Chip: ");
    Con_Print(sTypeNames[sCart.type]);
    Con_Putc('\n');
    if (!sCart.supported)
    {
        if (sCart.type == REPRO_CART_UNKNOWN)
            ShowChipIds();
        Fail("This board type is not\nsupported. Nothing was\nwritten.\n");
    }

    if (!Cfi_Read(&sCart, &sCfi))
    {
        Con_Print("CFI: no answer (");
        Con_Hex(sCfi.raw[0], 2);
        Con_Hex(sCfi.raw[1], 2);
        Con_Hex(sCfi.raw[2], 2);
        Con_Print(")\n");
        Fail("Can't read the chip's block\nlayout. Nothing was written.\n");
    }
    sBlock = sCfi.blockSize;
    Con_Print("Blocks: ");
    Con_Dec(sBlock / 1024);
    Con_Print(" KB, chip ");
    Con_Dec(sCfi.busSize >> 20);
    Con_Print(" MB\n");
    if (sBlock < MIN_BLOCK || sBlock > MAX_BLOCK || (sBlock & (sBlock - 1)) || sCfi.busSize < 0x1000000)
        Fail("This block layout is not\nsupported. Nothing was\nwritten.\n");
    sPiece = sBlock < PIECE_MAX ? sBlock : PIECE_MAX;

    for (i = 0; i < sCfg->numSpans; i++)
    {
        const struct FlasherSpan *sp = &sCfg->spans[i];
        u32 n = 0, newCount = 0, srcCount = 0, state;

        for (x = SpanFirst(sp); x < SpanEnd(sp); x += sBlock)
        {
            state = BlockState(x);
            n++;
            if (state == BLOCK_TARGET)
                newCount++;
            else if (state == BLOCK_SOURCE)
                srcCount++;
        }
        Con_Print(sp->name);
        Con_Print(": ");
        if (newCount == n)
        {
            Con_SetColor(COLOR_GREEN);
            Con_Print("done\n");
            Con_SetColor(COLOR_WHITE);
        }
        else
        {
            // Blocks in no expected state come from a run that was stopped.
            Con_Print(newCount + srcCount == n ? "to write\n" : "half written\n");
        }
    }
    CountBlocks(&total, &newBlocks, &x);
    if (newBlocks == total)
    {
        if (SpareClean())
        {
            Con_Print("\nAlready written. Nothing to\ndo.\n");
            Finish(0);
        }
        // Power was lost while the spare area was being erased again.
        Con_Print("Spare area still holds data\nfrom a stopped run: erase it.\n");
    }
    else
    {
        Con_Print("Will rewrite ");
        Con_Dec(total - newBlocks);
        Con_Print(total - newBlocks == 1 ? " block\n" : " blocks\n");
    }
    Con_Print("Save at ");
    Con_Hex(sCfg->saveAddr, 6);
    Con_Print(": not touched\n");
    Con_SetColor(COLOR_GRAY);
    Con_Print("Spare: ");
    Con_Hex(sCfg->stagingAddr, 6);
    Con_Print(", ");
    Con_Hex(sCfg->testAddr - MAX_BLOCK * 2, 6);
    Con_Putc('-');
    Con_Hex(sCfg->testAddr + MAX_BLOCK * 2 - 1, 6);
    Con_Putc('\n');
    Con_SetColor(COLOR_YELLOW);
    Con_Print("\nKeep the GBA powered on.\nTo write, hold L+R, press A.\nTo cancel, turn the GBA off.\n\n");
    Con_SetColor(COLOR_WHITE);

    WaitConfirm();

    for (;;)
    {
        rc = WriteAll();
        if (rc == REPRO_OK)
            break;
        Con_Putc('\n');
        Con_SetColor(COLOR_RED);
        Con_Print("Error ");
        Con_Hex(rc, 4);
        Con_Print(" at ");
        Con_Hex(sErrorAddr, 6);
        Con_Putc('\n');
        Con_Print(ErrorText(rc));
        Con_Putc('\n');
        if (rc == ERR_SIZE_TEST)
        {
            Con_Print("Chip error ");
            Con_Hex(sTestError, 4);
            Con_Putc('\n');
        }
        Con_SetColor(COLOR_WHITE);
        if (IsFatal(rc))
        {
            CountBlocks(&total, &newBlocks, &other);
            if (other == 0)
                Con_Print("Game data is intact.\n");
            Fail("Stopped. Turn the GBA off\nand report this screen.\n");
        }
        Con_Print("Press A to try again. Or\nturn off and send the\nflasher again later: it goes\non from where it stopped.\n");
        ErrorWait(rc);
    }

    Con_SetColor(COLOR_GREEN);
    Con_Print("\nDone. Turn the GBA off.\n");
    Finish(0);
}
