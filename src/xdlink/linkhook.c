// Emerald's link hook, in free cart ROM.
//
// On the copyright screen Emerald receives a program from the GameCube and
// runs it. Originally it swapped in its own Colosseum program when the
// received one had Colosseum's game code; the flasher changes that code to a
// call to LinkHook, which does the same swap and also prepares XD's programs:
//
//  1. LinkHook (before the program runs): when the received program is one of
//     XD's link programs, its loader is changed to jump to XdPostLoad instead
//     of the decompressed program.
//  2. XdPostLoad (after the loader has decompressed the program): checks the
//     result, copies the shim (xd_shim.c and the repro backend) to EWRAM,
//     redirects the program's flash and save calls to it and starts the
//     program.
//
// Anything that does not match exactly is left alone and runs as before.
//
// XD overwrites the header's game code ("TEST" on the disc) with its own value
// when it sends a program, and its loader keeps that value. So nothing here
// looks at the header: the received program is recognised by its compressed
// data and loader, and the decompressed one by everything after its header.

#include "repro_types.h"
#include "xd_link.h"
#include "shim_syms.h"

#define EWRAM               0x02000000
#define GAME_CODE           (*(vu32 *)(EWRAM + 0xAC))
#define COLOSSEUM_GAME_CODE 0x65366347  // "Gc6e"
#define HASH_BASIS          0x811C9DC5
// Which program LinkHook matched is passed to XdPostLoad in the loader's last
// word (its epilogue, never reached), which the stub copies to IWRAM with the
// rest of the loader. Nothing else touches it before XdPostLoad runs.
#define LOADER_IWRAM        0x03000000
#define XD_LOADER_INDEX     0x1C0
#define INDEX_MAGIC         0x584C0000  // "XL" in the upper half

extern const u8 gShimImage[];
extern const u8 gShimImageEnd[];
extern const u32 gColosseumProgram;     // ROM address of Emerald's Colosseum program
void CpuSet(const void *src, void *dest, u32 control);
void XdPostLoadArm(void);
void JumpToProgram(void);

static u32 Hash(const void *src, u32 bytes, u32 h)
{
    const u32 *p = src;

    for (bytes /= 4; bytes != 0; bytes--)
        h = (h ^ *p++) * 0x01000193;
    return h;
}

static void PrepareXd(void)
{
    u32 i, h;

    for (i = 0; i < gNumXdPrograms; i++)
    {
        const struct XdProgram *p = &gXdPrograms[i];
        u32 *loader = (u32 *)(EWRAM + p->loaderOffset);
        u32 *lits = (u32 *)((u8 *)loader + XD_LOADER_LITS);

        // The literals are a quick filter; the hash covers what runs.
        if (lits[0] != EWRAM + p->blobEnd || lits[1] != EWRAM + p->blobStart)
            continue;
        h = Hash((const void *)(EWRAM + p->blobStart), p->blobEnd - p->blobStart, HASH_BASIS);
        if (Hash(loader, XD_LOADER_SIZE, h) != p->recvHash)
            continue;
        loader[XD_LOADER_JUMP / 4] = 0xE59FF008;   // ldr pc, [pc, #8]
        loader[XD_LOADER_SPARE / 4] = (u32)XdPostLoadArm;
        loader[XD_LOADER_INDEX / 4] = INDEX_MAGIC | i;
        return;
    }
}

// Called by Emerald (through _call_via_r0) once a program has been received,
// in place of its Colosseum check.
void LinkHook(void)
{
    u32 code = GAME_CODE;

    if (code == COLOSSEUM_GAME_CODE)
    {
        // What Emerald did here: run its own Colosseum program instead.
        CpuSet((const void *)gColosseumProgram, (void *)EWRAM, 0x28000 / 2);
        GAME_CODE = code;
        return;
    }
    PrepareXd();
}

static void WriteBL(u32 site, u32 target)
{
    s32 off = (s32)(target & ~1) - (s32)(site + 4);

    ((vu16 *)site)[0] = 0xF000 | ((off >> 12) & 0x7FF);
    ((vu16 *)site)[1] = 0xF800 | ((off >> 1) & 0x7FF);
}

static const u32 sShimCalls[XD_CALL_COUNT] =
{
    [XD_CALL_IDENTIFY]   = SHIM_Xd_IdentifyFlash,
    [XD_CALL_READ]       = SHIM_Xd_ReadFlash,
    [XD_CALL_VERIFY]     = SHIM_Xd_VerifyFlashSector,
    [XD_CALL_VERIFY_N]   = SHIM_Xd_VerifyFlashSectorNBytes,
    [XD_CALL_SAVE_PHASE] = SHIM_Xd_SavePhase,
};

// Entered (through XdPostLoadArm) from XD's loader, with the program
// decompressed at 0x02000000 and interrupts off. Never returns.
void XdPostLoad(void)
{
    vu32 *mark = (vu32 *)(LOADER_IWRAM + XD_LOADER_INDEX);
    const struct XdProgram *p = NULL;
    u32 i;

    if ((*mark & 0xFFFF0000) == INDEX_MAGIC && (*mark & 0xFFFF) < gNumXdPrograms)
        p = &gXdPrograms[*mark & 0xFFFF];
    *mark = 0;

    if (p != NULL && Hash((const void *)(EWRAM + XD_HEADER_SIZE), p->decSize - XD_HEADER_SIZE, HASH_BASIS) == p->decHash)
    {
        const u8 *src = gShimImage;
        u8 *dst = (u8 *)SHIM___shim_start;

        while (src < gShimImageEnd)
            *dst++ = *src++;
        while ((u32)dst < SHIM___shim_bss_end)
            *dst++ = 0;
        *(struct XdParams *)SHIM_gXdParams = p->params;
        for (i = 0; i < p->numSites; i++)
            WriteBL(p->sites[i].addr, sShimCalls[p->sites[i].call]);
    }
    JumpToProgram();
}
