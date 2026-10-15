#ifndef GUARD_REPRO_CART_H
#define GUARD_REPRO_CART_H

// Board and flash chip handling for bootleg "batteryless" Pokémon carts.
//
// These carts keep a 128 KiB working copy of the save in banked SRAM
// (bank selected by a byte write to 0x09000000) and persist it to a 128 KiB
// area of the ROM flash chip. The board types, probe order, bank values and
// chip command sequences below mirror the batteryless code found on the
// AliExpress carts (as extracted by DanOl98's pkmn_gba_batteryless_patcher).

#include "repro_types.h"

enum
{
    REPRO_CART_UNKNOWN,
    REPRO_CART_INTEL_BUFFERED, // Intel command set, 1 KiB buffered programming (cmd 0xEA)
    REPRO_CART_INTEL_WORD,     // Intel command set, word programming (cmd 0x40)
    REPRO_CART_AMD_SWAPPED,    // AMD command set, x16, data lines D0/D1 swapped
    REPRO_CART_AMD_DUAL,       // AMD command set, two x8 chips side by side
    REPRO_CART_AMD_X16,        // AMD command set, x16 (the cart's own code saves only 64 KiB)
    REPRO_CART_TYPE_COUNT
};

struct ReproCart
{
    u8 type;
    u8 bankValue[2];    // values written to 0x09000000 by the flush routine
    u8 supported;       // FALSE when writes are refused for this board type
    u32 eraseStride;    // erase commands are issued every eraseStride bytes of the save area
    u32 programUnit;    // bytes programmed per program operation (2 or 1024)
    u32 unlockAddr1;    // AMD: byte offsets from the start of ROM
    u32 unlockAddr2;
    u16 unlockData1;
    u16 unlockData2;
    u16 cmdMultiplier;  // AMD: 0x0001 for one chip, 0x0101 to send a command to both lanes
    u16 statusMask;     // AMD: DQ7 bits checked when polling an erase
    u8 useBypass;       // AMD: program with unlock bypass
    u8 bufferCmd;       // Intel buffered: buffer program command
};

// Values the cart's own boot code uses to select SRAM banks 0 and 1.
#define REPRO_BOOT_BANK0 0
#define REPRO_BOOT_BANK1 1

#define REPRO_OK              0
#define REPRO_ERR_TIMEOUT     0x8101
#define REPRO_ERR_STATUS      0x8102
#define REPRO_ERR_VERIFY      0x8103
#define REPRO_ERR_UNSUPPORTED 0x8104
#define REPRO_ERR_SRAM        0x8105
#define REPRO_ERR_LOCKED      0x8106

const struct ReproCart *ReproCart_Probe(void);
const struct ReproCart *ReproCart_Get(u8 type);
u16 ReproCart_EraseBlock(const struct ReproCart *cart, u32 romOffset);
// Programs cart->programUnit bytes at romOffset; 'unitOffset' is the
// position of that unit inside the 128 KiB SRAM working copy.
u16 ReproCart_ProgramUnit(const struct ReproCart *cart, u32 romOffset, u32 unitOffset);
void ReproCart_ReadArrayMode(const struct ReproCart *cart);

// SRAM working copy (128 KiB in two 64 KiB banks)
void ReproSram_ForgetBank(void);
void ReproSram_SelectBank(u32 bank, const u8 *bankValues);
u8 ReproSram_Read8(u32 offset, const u8 *bankValues);
void ReproSram_Write8(u32 offset, u8 value, const u8 *bankValues);

#endif // GUARD_REPRO_CART_H
