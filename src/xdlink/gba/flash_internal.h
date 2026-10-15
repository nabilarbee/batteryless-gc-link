#ifndef GUARD_GBA_FLASH_INTERNAL_H
#define GUARD_GBA_FLASH_INTERNAL_H

// Stand-in for payload/include/gba/flash_internal.h when the repro backend is
// built into the XD shim: the flash library's variables are the ones inside
// XD's program, at the addresses in gXdParams.

#include "xd_params.h"

struct FlashSector
{
    u32 size;
    u8 shift;
    u16 count;
    u16 top;
};

struct FlashType {
    u32 romSize;
    struct FlashSector sector;
    u16 wait[2]; // game pak bus read/write wait
    union {
        struct {
        u8 makerId;
        u8 deviceId;
        } separate;
        u16 joined;
    } ids;
};

#define ProgramFlashByte   (*(u16 (**)(u16, u32, u8))gXdParams.programFlashByte)
#define ProgramFlashSector (*(u16 (**)(u16, void *))gXdParams.programFlashSector)
#define EraseFlashChip     (*(u16 (**)(void))gXdParams.eraseFlashChip)
#define EraseFlashSector   (*(u16 (**)(u16))gXdParams.eraseFlashSector)
#define WaitForFlashWrite  (*(u16 (**)(u8, u8 *, u8))gXdParams.waitForFlashWrite)
#define gFlashMaxTime      (*(const u16 **)gXdParams.flashMaxTime)
#define gFlash             (*(const struct FlashType **)gXdParams.flash)

#endif // GUARD_GBA_FLASH_INTERNAL_H
