// CFI (Common Flash Interface) query of the cart's flash chip(s), used to
// learn the erase block size before anything is erased.
#include "repro_types.h"
#include "repro_hw.h"
#include "repro_cart.h"
#include "cfi.h"

static u8 Unswap(u8 v)
{
    return (v & 0xFC) | ((v & 1) << 1) | ((v >> 1) & 1);
}

// Sends the CFI query for this board. Returns the distance in bytes between
// consecutive CFI items, or 0 for boards without a known query.
static u32 EnterCfi(const struct ReproCart *cart, u32 *lanes)
{
    switch (cart->type)
    {
    case REPRO_CART_INTEL_BUFFERED:
    case REPRO_CART_INTEL_WORD:
        // x16 Intel: 0x98 at any address; 0x98 is the same with D0/D1 swapped.
        ROM_WRITE16(0, 0xFF);
        ROM_WRITE16(0, 0x98);
        *lanes = 1;
        return 2;
    case REPRO_CART_AMD_SWAPPED:
        // x16 AMD: 0x98 at word address 0x55.
        ROM_WRITE16(0, 0xF0);
        ROM_WRITE16(0xAA, 0x98);
        *lanes = 1;
        return 2;
    case REPRO_CART_AMD_DUAL:
        // Two x8 chips: 0x98 at byte address 0xAA of each chip, which the
        // board maps to halfword 0xAA of the bus. Items sit at chip byte
        // addresses 2n, so bus byte offsets 4n.
        ROM_WRITE16(0, 0xF0F0);
        ROM_WRITE16(0x154, 0x9898);
        *lanes = 2;
        return 4;
    }
    return 0;
}

static u8 RawItem(u32 n, u32 stride, u32 lane)
{
    return ROM_READ16(n * stride) >> (lane * 8);
}

static u8 Item(u32 n, u32 stride, u32 lane, bool32 swapped)
{
    u8 v = RawItem(n, stride, lane);
    return swapped ? Unswap(v) : v;
}

// Decodes one lane. Returns FALSE if the answer is not a consistent CFI table.
static bool32 ReadLane(u32 stride, u32 lane, struct CfiInfo *info, bool32 *swapped)
{
    u8 q = RawItem(0x10, stride, lane);
    u8 r = RawItem(0x11, stride, lane);
    u8 y = RawItem(0x12, stride, lane);
    u32 i, total, sizeLog2, count, size, field;

    if (lane == 0)
    {
        info->raw[0] = q;
        info->raw[1] = r;
        info->raw[2] = y;
    }

    if (q == 'Q' && r == 'R' && y == 'Y')
        *swapped = FALSE;
    else if (q == Unswap('Q') && r == Unswap('R') && y == Unswap('Y'))
        *swapped = TRUE;
    else
        return FALSE;

    sizeLog2 = Item(0x27, stride, lane, *swapped);
    info->numRegions = Item(0x2C, stride, lane, *swapped);
    if (sizeLog2 < 16 || sizeLog2 > 26 || info->numRegions == 0 || info->numRegions > CFI_MAX_REGIONS)
        return FALSE;
    info->sizeLog2 = sizeLog2;

    total = 0;
    for (i = 0; i < info->numRegions; i++)
    {
        count = Item(0x2D + i * 4, stride, lane, *swapped) | (Item(0x2E + i * 4, stride, lane, *swapped) << 8);
        field = Item(0x2F + i * 4, stride, lane, *swapped) | (Item(0x30 + i * 4, stride, lane, *swapped) << 8);
        count += 1;
        size = field ? field * 256 : 128;
        if (size & (size - 1))
            return FALSE;
        info->regionBlocks[i] = count;
        info->regionSize[i] = size;
        total += count * size;
    }

    return total == (1u << sizeLog2);
}

bool32 Cfi_Read(const struct ReproCart *cart, struct CfiInfo *info)
{
    u16 ime;
    u16 header0 = ROM_READ16(0);
    u16 header1 = ROM_READ16(2);
    u32 lanes = 0;
    u32 stride;
    u32 i, largest;
    bool32 ok = FALSE;
    bool32 swapped;
    struct CfiInfo other;

    for (i = 0; i < sizeof(*info); i++)
        ((u8 *)info)[i] = 0;

    IRQ_DISABLE(ime);
    stride = EnterCfi(cart, &lanes);
    if (stride)
    {
        swapped = FALSE;
        ok = ReadLane(stride, 0, info, &swapped);
        if (swapped)
            info->swapped |= 1;
        if (ok && lanes == 2)
        {
            // Both chips must describe the same layout.
            swapped = FALSE;
            ok = ReadLane(stride, 1, &other, &swapped);
            if (swapped)
                info->swapped |= 2;
            if (ok && (other.sizeLog2 != info->sizeLog2 || other.numRegions != info->numRegions))
                ok = FALSE;
            for (i = 0; ok && i < info->numRegions; i++)
            {
                if (other.regionBlocks[i] != info->regionBlocks[i] || other.regionSize[i] != info->regionSize[i])
                    ok = FALSE;
            }
        }
        ReproCart_ReadArrayMode(cart);
    }
    if (ROM_READ16(0) != header0 || ROM_READ16(2) != header1)
    {
        ReproCart_ReadArrayMode(cart);
        if (ROM_READ16(0) != header0 || ROM_READ16(2) != header1)
            ok = FALSE;
    }
    IRQ_RESTORE(ime);

    if (!ok)
        return FALSE;

    largest = 0;
    for (i = 0; i < info->numRegions; i++)
    {
        if (info->regionSize[i] > largest)
            largest = info->regionSize[i];
    }
    info->lanes = lanes;
    info->blockSize = largest * lanes;
    info->busSize = (1u << info->sizeLog2) * lanes;
    info->ok = TRUE;
    return TRUE;
}
