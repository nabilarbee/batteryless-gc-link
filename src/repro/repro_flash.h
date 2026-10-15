#ifndef GUARD_REPRO_FLASH_H
#define GUARD_REPRO_FLASH_H

// Save backend for batteryless repro carts.
//
// Repro carts have no 128 KiB flash save chip. Their game keeps the save as
// a normal FLASH1M image (32 sectors of 4 KiB) inside the cart's ROM flash.
// This backend finds that image in ROM and serves the AgbFlash API from it,
// so libpmagb/save.c works unchanged.

#include "repro_types.h"

#define REPRO_SECTOR_SIZE   0x1000
#define REPRO_NUM_SECTORS   32
#define REPRO_SAVE_SIZE     (REPRO_SECTOR_SIZE * REPRO_NUM_SECTORS)

#define REPRO_ROM_START     0x08000000
#define REPRO_ROM_MAX_SIZE  0x02000000

// The save area is expected on a 64 KiB boundary, which is the erase-block
// alignment every known repro batteryless patch uses.
#define REPRO_SCAN_STEP     0x10000

#define REPRO_SIGNATURE     0x08012025

// Error codes returned through the AgbFlash API. Any nonzero value is a failure.
#define REPRO_ERR_READ_ONLY 0x80FE
#define REPRO_ERR_RANGE     0x80FF

// Result of ReproScanForSave
struct ReproScanResult
{
    u32 saveOffset;     // offset of the save area from the start of cart ROM
    u32 newestCounter;  // highest save counter of a complete slot
    u8 numCandidates;   // distinct (non-mirror) save areas found
    u8 completeSlots;   // bit 0: slot A complete, bit 1: slot B complete
};

extern u8 gReproActive;     // nonzero while the save is served from cart ROM
extern u32 gReproSaveOffset;
extern u8 gReproSession;    // 0 = reading ROM, 1 = SRAM working copy ready, 2 = writes refused
extern u8 gReproCartType;   // REPRO_CART_* from repro_cart.h
extern u16 gReproLastError;

// Core scan, also built on the host for tests.
// 'rom' points at the first byte of cart ROM, 'romSize' is the readable size.
bool32 ReproScanForSave(const u8 *rom, u32 romSize, struct ReproScanResult *result);
u32 ReproGetRomSize(const u8 *rom);

#ifndef REPRO_HOST_TEST
bool32 ReproFlash_Detect(void);
void ReproFlash_Read(u16 sectorNum, u32 offset, u8 *dest, u32 size);
u32 ReproFlash_Verify(u16 sectorNum, const u8 *src, u32 size);
u16 ReproFlash_ProgramByte(u16 sectorNum, u32 offset, u8 data);
u16 ReproFlash_ProgramSector(u16 sectorNum, void *src);
u16 ReproFlash_EraseSector(u16 sectorNum);
u16 ReproFlash_EraseChip(void);
u16 ReproFlash_WaitForWrite(u8 phase, u8 *addr, u8 lastData);
u16 ReproFlash_Commit(void);
#endif

#endif // GUARD_REPRO_FLASH_H
