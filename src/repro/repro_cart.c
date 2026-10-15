// Board detection and flash commands for bootleg batteryless Pokémon carts.
// See include/repro_cart.h for where the board types come from.

#include "repro_types.h"
#include "repro_hw.h"
#include "repro_cart.h"

// Upper bound on status polls before giving up (about 10 s of polling on hardware).
#define POLL_LIMIT 0x00800000

#define AMD_X16_UNLOCK1  0xAAA   // word address 0x555
#define AMD_X16_UNLOCK2  0x554   // word address 0x2AA
#define AMD_X8_UNLOCK1   0x1554  // byte address 0xAAA on each x8 chip
#define AMD_X8_UNLOCK2   0xAAA   // byte address 0x555 on each x8 chip

static const struct ReproCart sCarts[REPRO_CART_TYPE_COUNT] =
{
    [REPRO_CART_UNKNOWN] =
    {
        .type = REPRO_CART_UNKNOWN,
        .supported = FALSE,
    },
    [REPRO_CART_INTEL_BUFFERED] =
    {
        .type = REPRO_CART_INTEL_BUFFERED,
        .bankValue = {0, 1},
        .supported = TRUE,
        .eraseStride = 0x20000,
        .programUnit = 0x400,
        .bufferCmd = 0xEA,
    },
    [REPRO_CART_INTEL_WORD] =
    {
        .type = REPRO_CART_INTEL_WORD,
        .bankValue = {0, 1},
        .supported = TRUE,
        .eraseStride = 0x10000,
        .programUnit = 2,
    },
    [REPRO_CART_AMD_SWAPPED] =
    {
        .type = REPRO_CART_AMD_SWAPPED,
        .bankValue = {0xEE, 0xEF},
        .supported = TRUE,
        .eraseStride = 0x10000,
        .programUnit = 2,
        .unlockAddr1 = AMD_X16_UNLOCK1,
        .unlockAddr2 = AMD_X16_UNLOCK2,
        .unlockData1 = 0xA9, // 0xAA with D0/D1 swapped
        .unlockData2 = 0x56, // 0x55 with D0/D1 swapped
        .cmdMultiplier = 0x0001,
        .statusMask = 0x0080,
        .useBypass = FALSE,
    },
    [REPRO_CART_AMD_DUAL] =
    {
        .type = REPRO_CART_AMD_DUAL,
        .bankValue = {0, 1},
        .supported = TRUE,
        .eraseStride = 0x20000,
        .programUnit = 2,
        .unlockAddr1 = AMD_X8_UNLOCK1,
        .unlockAddr2 = AMD_X8_UNLOCK2,
        .unlockData1 = 0xAAA9, // high chip 0xAA, low chip 0xAA with D0/D1 swapped
        .unlockData2 = 0x5556,
        .cmdMultiplier = 0x0101,
        .statusMask = 0x8080,
        .useBypass = TRUE,
    },
    [REPRO_CART_AMD_X16] =
    {
        // The cart's own code only ever saves the first 64 KiB on this board,
        // so its bank handling is unknown. Refuse writes.
        .type = REPRO_CART_AMD_X16,
        .bankValue = {0, 1},
        .supported = FALSE,
        .eraseStride = 0x20000,
        .programUnit = 2,
        .unlockAddr1 = AMD_X16_UNLOCK1,
        .unlockAddr2 = AMD_X16_UNLOCK2,
        .unlockData1 = 0xAAA9,
        .unlockData2 = 0x5556,
        .cmdMultiplier = 0x0101,
        .statusMask = 0x8080,
        .useBypass = TRUE,
    },
};

// Device IDs (word 1 in Intel identifier mode) and the board type they select.
static const u16 sIntelBufferedIds[] = {0x8902, 0x8904, 0x887D, 0x88B0};
static const u16 sIntelWordIds[] =
{
    0x8815, 0x8810, 0x880E, 0x8813, 0x880C, 0x880F, 0x8812,
    0x8816, 0x8855, 0x8857, 0x88C6, 0x88C4, 0x880D,
};

#define AMD_SWAPPED_DEVICE_ID 0x227D
#define CFI_QR_BOTH_LANES     0x5152

const struct ReproCart *ReproCart_Get(u8 type)
{
    if (type >= REPRO_CART_TYPE_COUNT)
        type = REPRO_CART_UNKNOWN;
    return &sCarts[type];
}

static bool32 InList(u16 id, const u16 *list, u32 count)
{
    u32 i;

    for (i = 0; i < count; i++)
    {
        if (list[i] == id)
            return TRUE;
    }

    return FALSE;
}

static bool32 IsIntel(const struct ReproCart *cart)
{
    return cart->type == REPRO_CART_INTEL_BUFFERED || cart->type == REPRO_CART_INTEL_WORD;
}

// Returns any supported chip to read-array mode: AMD reset on both byte
// lanes, then Intel read array (an AMD chip in read mode ignores 0xFF).
static void ResetAnyChip(void)
{
    ROM_WRITE16(0, 0xF0F0);
    ROM_WRITE16(0, 0xFFFF);
}

void ReproCart_ReadArrayMode(const struct ReproCart *cart)
{
    if (cart->type == REPRO_CART_UNKNOWN)
        ResetAnyChip();
    else if (IsIntel(cart))
        ROM_WRITE16(0, 0xFF);
    else
        ROM_WRITE16(0, 0xF0 * cart->cmdMultiplier);
}

// Identifies the board with the same probe sequence as the cart's own code,
// except that an unrecognised chip is refused rather than guessed.
const struct ReproCart *ReproCart_Probe(void)
{
    u16 ime;
    u16 id;
    u16 header0, header1;
    u16 auto0, auto1;
    u8 type = REPRO_CART_UNKNOWN;

    IRQ_DISABLE(ime);

    header0 = ROM_READ16(0);
    header1 = ROM_READ16(2);

    // Intel: clear status, read identifier.
    ROM_WRITE16(0, 0xFF);
    ROM_WRITE16(0, 0x50);
    ROM_WRITE16(0, 0x90);
    id = ROM_READ16(2);

    if (InList(id, sIntelBufferedIds, sizeof(sIntelBufferedIds) / sizeof(sIntelBufferedIds[0])))
    {
        ROM_WRITE16(0, 0xFF);
        type = REPRO_CART_INTEL_BUFFERED;
        goto done;
    }

    if (InList(id, sIntelWordIds, sizeof(sIntelWordIds) / sizeof(sIntelWordIds[0])))
    {
        ROM_WRITE16(0, 0xFF);
        type = REPRO_CART_INTEL_WORD;
        goto done;
    }

    // An AMD chip ignores a lone 0x90, so word 1 still reads as cart header.
    // Anything else is an Intel-style chip this code does not know: stop
    // here rather than send it AMD commands.
    if (id != header1)
    {
        ROM_WRITE16(0, 0xFF);
        goto done;
    }

    ROM_WRITE16(0, 0xF0);

    // AMD autoselect, x16, with D0/D1 swapped unlock data.
    ROM_WRITE16(AMD_X16_UNLOCK1, 0xA9);
    ROM_WRITE16(AMD_X16_UNLOCK2, 0x56);
    ROM_WRITE16(AMD_X16_UNLOCK1, 0x90);
    auto0 = ROM_READ16(0);
    auto1 = ROM_READ16(2);
    ROM_WRITE16(0, 0xF0);

    if (auto1 == AMD_SWAPPED_DEVICE_ID)
    {
        type = REPRO_CART_AMD_SWAPPED;
        goto done;
    }

    // CFI query, two x8 chips in byte mode.
    ROM_WRITE16(0, 0xF0F0);
    ROM_WRITE16(0x154, 0x9898);
    id = ROM_READ16(0x40);
    ROM_WRITE16(0, 0xF0F0);
    if (id == CFI_QR_BOTH_LANES)
    {
        type = REPRO_CART_AMD_DUAL;
        goto done;
    }

    // CFI query, x16.
    ROM_WRITE16(0xAA, 0x9898);
    id = ROM_READ16(0x20);
    ROM_WRITE16(0, 0xF0F0);
    if (id == CFI_QR_BOTH_LANES)
    {
        type = REPRO_CART_AMD_X16;
        goto done;
    }

    ROM_WRITE16(0, 0xF0);

    // The cart's own code falls back to the swapped AMD board here. Only do
    // the same when the chip visibly answered the swapped autoselect command.
    if (auto0 != header0 || auto1 != header1)
        type = REPRO_CART_AMD_SWAPPED;

done:
    ReproCart_ReadArrayMode(&sCarts[type]);

    // The chip must be back in read-array mode before anything continues.
    if (ROM_READ16(0) != header0 || ROM_READ16(2) != header1)
    {
        ResetAnyChip();
        type = REPRO_CART_UNKNOWN;
    }

    IRQ_RESTORE(ime);

    return &sCarts[type];
}

// ---------------------------------------------------------------- SRAM

// Last value written to the bank register, or -1 when unknown. Avoids
// writing the register more often than the cart's own code does.
static s32 sBankRegister = -1;

void ReproSram_ForgetBank(void)
{
    sBankRegister = -1;
}

void ReproSram_SelectBank(u32 bank, const u8 *bankValues)
{
    u8 value = bankValues[bank & 1];

    if (sBankRegister != value)
    {
        BANK_WRITE8(value);
        sBankRegister = value;
    }
}

u8 ReproSram_Read8(u32 offset, const u8 *bankValues)
{
    ReproSram_SelectBank(offset >> 16, bankValues);
    return SRAM_READ8(offset & 0xFFFF);
}

void ReproSram_Write8(u32 offset, u8 value, const u8 *bankValues)
{
    ReproSram_SelectBank(offset >> 16, bankValues);
    SRAM_WRITE8(offset & 0xFFFF, value);
}

static u16 SramWord(u32 offset, const u8 *bankValues)
{
    ReproSram_SelectBank(offset >> 16, bankValues);
    return SRAM_READ8(offset & 0xFFFF) | (SRAM_READ8((offset + 1) & 0xFFFF) << 8);
}

// ---------------------------------------------------------------- AMD

static void AmdUnlock(const struct ReproCart *cart)
{
    ROM_WRITE16(cart->unlockAddr1, cart->unlockData1);
    ROM_WRITE16(cart->unlockAddr2, cart->unlockData2);
}

// Waits for DQ7 of one byte lane to read 1 (erase complete), checking DQ5.
static bool32 AmdWaitLane(u32 romOffset, u16 dq7)
{
    u32 polls;
    u16 dq5 = dq7 >> 2;
    u16 value;

    for (polls = 0; polls < POLL_LIMIT; polls++)
    {
        value = ROM_READ16(romOffset);
        if (value & dq7)
            return TRUE;
        if (value & dq5)
            return (ROM_READ16(romOffset) & dq7) != 0;
    }

    return FALSE;
}

static u16 AmdEraseBlock(const struct ReproCart *cart, u32 romOffset)
{
    u16 result = REPRO_OK;
    u16 m = cart->cmdMultiplier;

    ROM_WRITE16(romOffset, 0xF0 * m);
    AmdUnlock(cart);
    ROM_WRITE16(cart->unlockAddr1, 0x80 * m);
    AmdUnlock(cart);
    ROM_WRITE16(romOffset, 0x30 * m);

    if ((cart->statusMask & 0x8000) && !AmdWaitLane(romOffset, 0x8000))
        result = REPRO_ERR_TIMEOUT;
    if (result == REPRO_OK && (cart->statusMask & 0x0080) && !AmdWaitLane(romOffset, 0x0080))
        result = REPRO_ERR_TIMEOUT;

    ROM_WRITE16(romOffset, 0xF0 * m);

    if (result == REPRO_OK && ROM_READ16(romOffset) != 0xFFFF)
        result = REPRO_ERR_VERIFY;

    return result;
}

// Data polling: done when the word reads back as written. A lane that has
// not finished and shows DQ5 has exceeded its time limit.
static u16 AmdWaitProgram(const struct ReproCart *cart, u32 romOffset, u16 value)
{
    u32 polls;
    u32 shift;

    for (polls = 0; polls < POLL_LIMIT; polls++)
    {
        u16 read = ROM_READ16(romOffset);

        if (read == value)
            return REPRO_OK;

        for (shift = 0; shift < 16; shift += 8)
        {
            u16 laneMask = 0xFF << shift;

            if (!(cart->statusMask & (0x80 << shift)))
                continue;
            if ((read & laneMask) != (value & laneMask) && (read & (0x20 << shift)))
            {
                if ((ROM_READ16(romOffset) & laneMask) != (value & laneMask))
                    return REPRO_ERR_STATUS;
            }
        }
    }

    return REPRO_ERR_TIMEOUT;
}

static u16 AmdProgramWords(const struct ReproCart *cart, u32 romOffset, u32 unitOffset, u32 size)
{
    u16 result = REPRO_OK;
    u16 m = cart->cmdMultiplier;
    u32 i;

    for (i = 0; i < size && result == REPRO_OK; i += 2)
    {
        u16 value = SramWord(unitOffset + i, cart->bankValue);

        if (value == 0xFFFF)
            continue; // erased state, nothing to program

        if (cart->useBypass)
        {
            ROM_WRITE16(romOffset + i, 0xA0 * m);
        }
        else
        {
            AmdUnlock(cart);
            ROM_WRITE16(cart->unlockAddr1, 0xA0 * m);
        }
        ROM_WRITE16(romOffset + i, value);
        result = AmdWaitProgram(cart, romOffset + i, value);
    }

    return result;
}

static u16 AmdProgramUnit(const struct ReproCart *cart, u32 romOffset, u32 unitOffset)
{
    u16 result;
    u16 m = cart->cmdMultiplier;

    if (cart->useBypass)
    {
        AmdUnlock(cart);
        ROM_WRITE16(cart->unlockAddr1, 0x20 * m);
    }

    result = AmdProgramWords(cart, romOffset, unitOffset, cart->programUnit);

    // A timed-out operation only accepts a reset.
    if (result != REPRO_OK)
        ROM_WRITE16(romOffset, 0xF0 * m);

    if (cart->useBypass)
    {
        // Leave unlock bypass mode.
        ROM_WRITE16(romOffset, 0x90 * m);
        ROM_WRITE16(romOffset, 0x00);
    }
    ROM_WRITE16(romOffset, 0xF0 * m);

    return result;
}

// ---------------------------------------------------------------- Intel

#define INTEL_SR_READY  0x80
// Erase error, program error and VPP error. A locked block also sets one of
// the first two. Bits 0 and 1 are left out: their meaning varies by chip and
// they trade places on boards with D0/D1 swapped.
#define INTEL_SR_ERRORS 0x38

static u16 IntelWaitReady(u32 romOffset)
{
    u32 polls;
    u8 status;

    for (polls = 0; polls < POLL_LIMIT; polls++)
    {
        status = ROM_READ16(romOffset);
        if (status & INTEL_SR_READY)
        {
            if (status & INTEL_SR_ERRORS)
            {
                ROM_WRITE16(romOffset, 0x50); // clear status
                return REPRO_ERR_STATUS;
            }
            return REPRO_OK;
        }
    }

    return REPRO_ERR_TIMEOUT;
}

static u16 IntelEraseBlock(u32 romOffset)
{
    u16 result;
    u32 polls;

    // Unlock the block, then wait until its lock bits read clear.
    ROM_WRITE16(romOffset, 0xFF);
    ROM_WRITE16(romOffset, 0x50);
    ROM_WRITE16(romOffset, 0x60);
    ROM_WRITE16(romOffset, 0xD0);
    ROM_WRITE16(romOffset, 0x90);
    for (polls = 0; polls < POLL_LIMIT; polls++)
    {
        if ((ROM_READ16(romOffset + 2) & 3) == 0)
            break;
    }
    ROM_WRITE16(romOffset, 0xFF);
    if (polls == POLL_LIMIT)
        return REPRO_ERR_LOCKED;

    ROM_WRITE16(romOffset, 0x20);
    ROM_WRITE16(romOffset, 0xD0);
    result = IntelWaitReady(romOffset);
    ROM_WRITE16(romOffset, 0xFF);

    if (result == REPRO_OK && ROM_READ16(romOffset) != 0xFFFF)
        result = REPRO_ERR_VERIFY;

    return result;
}

static u16 IntelProgramUnit(const struct ReproCart *cart, u32 romOffset, u32 unitOffset)
{
    u16 result = REPRO_OK;
    u32 i;

    if (cart->type == REPRO_CART_INTEL_BUFFERED)
    {
        u32 words = cart->programUnit / 2;

        // Same sequence as the cart's own code: wait ready, buffer command,
        // word count - 1, data, confirm at the last address.
        ROM_WRITE16(romOffset, 0xFF);
        ROM_WRITE16(romOffset, 0x70);
        result = IntelWaitReady(romOffset);
        ROM_WRITE16(romOffset, 0xFF);
        if (result != REPRO_OK)
            return result;
        ROM_WRITE16(romOffset, cart->bufferCmd);
        ROM_WRITE16(romOffset, words - 1);
        for (i = 0; i < words; i++)
            ROM_WRITE16(romOffset + i * 2, SramWord(unitOffset + i * 2, cart->bankValue));
        ROM_WRITE16(romOffset + (words - 1) * 2, 0xD0);
        result = IntelWaitReady(romOffset + (words - 1) * 2);
        ROM_WRITE16(romOffset, 0xFF);
        return result;
    }

    for (i = 0; i < cart->programUnit && result == REPRO_OK; i += 2)
    {
        u16 value = SramWord(unitOffset + i, cart->bankValue);

        if (value == 0xFFFF)
            continue;

        ROM_WRITE16(romOffset + i, 0xFF);
        ROM_WRITE16(romOffset + i, 0x70);
        result = IntelWaitReady(romOffset + i);
        ROM_WRITE16(romOffset + i, 0xFF);
        if (result != REPRO_OK)
            break;
        ROM_WRITE16(romOffset + i, 0x40);
        ROM_WRITE16(romOffset + i, value);
        result = IntelWaitReady(romOffset + i);
        ROM_WRITE16(romOffset + i, 0xFF);
    }

    return result;
}

// ---------------------------------------------------------------- dispatch

u16 ReproCart_EraseBlock(const struct ReproCart *cart, u32 romOffset)
{
    u16 ime;
    u16 result;

    if (!cart->supported)
        return REPRO_ERR_UNSUPPORTED;

    IRQ_DISABLE(ime);
    if (IsIntel(cart))
        result = IntelEraseBlock(romOffset);
    else
        result = AmdEraseBlock(cart, romOffset);
    IRQ_RESTORE(ime);

    return result;
}

u16 ReproCart_ProgramUnit(const struct ReproCart *cart, u32 romOffset, u32 unitOffset)
{
    u16 ime;
    u16 result;

    if (!cart->supported)
        return REPRO_ERR_UNSUPPORTED;

    IRQ_DISABLE(ime);
    if (IsIntel(cart))
        result = IntelProgramUnit(cart, romOffset, unitOffset);
    else
        result = AmdProgramUnit(cart, romOffset, unitOffset);
    IRQ_RESTORE(ime);

    return result;
}
