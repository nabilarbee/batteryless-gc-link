#ifndef GUARD_CFI_H
#define GUARD_CFI_H

#include "repro_cart.h"

#define CFI_MAX_REGIONS 4

struct CfiInfo
{
    u8 ok;
    u8 lanes;           // chips side by side on the 16-bit bus (1 or 2)
    u8 swapped;         // bit n: lane n answers with D0/D1 swapped
    u8 numRegions;
    u8 sizeLog2;        // per chip
    u8 raw[3];          // what lane 0 returned for "QRY", for diagnostics
    u32 blockSize;      // largest erase block, in bytes of the GBA address space
    u32 busSize;        // bytes of the GBA address space the chip(s) cover
    u16 regionBlocks[CFI_MAX_REGIONS];
    u32 regionSize[CFI_MAX_REGIONS];   // per chip
};

// Reads the erase block layout with a CFI query sent the way the board
// expects (lane layout, D0/D1 swap). Leaves the chip in read-array mode.
bool32 Cfi_Read(const struct ReproCart *cart, struct CfiInfo *info);

#endif // GUARD_CFI_H
