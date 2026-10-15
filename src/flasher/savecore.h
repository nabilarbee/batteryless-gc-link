#ifndef GUARD_SAVECORE_H
#define GUARD_SAVECORE_H

// Save area writing shared by the save writer (savewriter.c) and the
// batteryless dumper (bldumper.c): board probe, CFI, writing the save area
// from a RAM copy through the cart's SRAM, and the save summary on screen.

#include "repro_cart.h"
#include "cfi.h"

#define SAVE_SIZE        0x20000
#define SECTOR_SIZE      0x1000
#define SECTORS_PER_SLOT 14
#define SECTOR_SIGNATURE 0x08012025
#define MIN_BLOCK        0x1000
#define MAX_BLOCK        0x40000

#define KEY_A 0x0001
#define KEY_B 0x0002
#define KEY_R 0x0100
#define KEY_L 0x0200

// Errors of these programs; the chip layer uses 0x81xx (repro_cart.h).
#define ERR_WRITE_VERIFY  0x8204

struct SaveInfo
{
    bool8 valid;
    u8 name[8];
    u16 trainerId;
    u16 hours;
    u8 minutes;
    u32 counter;
};

extern u32 (*gHashWords)(const void *src, u32 bytes, u32 h);
extern struct ReproCart gCart;
extern struct CfiInfo gCfi;
extern u32 gBlock;
extern u32 gFFHash;
extern u32 gErrorAddr;
extern bool8 gWriteStarted;

// The test harness replaces these three.
void WaitVBlank(void);
u16 ReadKeys(void);
void ErrorWait(u32 code);

void WaitFrames(u32 frames);
void PrintOk(void);
void PrintDecPadded(u32 value, u32 digits);

void SaveCore_Init(void);
u32 ChunkHash(u32 off);
bool32 IsErased(u32 addr, u32 len);

// Probes the board and reads its CFI table, printing what it finds. Returns
// NULL when writing is possible, else the reason it is not.
const char *ProbeChip(void);

// Whether the 128 KiB save area at saveAddr already holds 'save'.
bool32 SaveWritten(const u8 *save, u32 saveAddr);
// Erases the save area, writes 'save' into it and checks it (one retry).
u16 WriteSave(const u8 *save, u32 saveAddr);

void ReadSaveInfo(const u8 *save, struct SaveInfo *info, bool32 emeraldChecksums);
void PrintSaveInfo(const char *label, const struct SaveInfo *info);

void PrintError(u16 rc);

#endif // GUARD_SAVECORE_H
