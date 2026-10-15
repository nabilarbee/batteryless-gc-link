#ifndef GUARD_XD_LINK_H
#define GUARD_XD_LINK_H

#include "repro_types.h"
#include "xd_params.h"

#define XD_MAX_SITES 8

// The IWRAM loader at the end of each XD link program (identical in all of
// them): it moves the compressed program to the top of EWRAM, decompresses it
// to 0x02000000 and jumps there.
#define XD_LOADER_SIZE  0x1C4
#define XD_LOADER_JUMP  0x1AC   // "bx r5", the jump to 0x02000000
#define XD_LOADER_SPARE 0x1BC   // never reached; holds the new jump target
#define XD_LOADER_LITS  0x1B4   // blob end, blob start
#define XD_HEADER_SIZE  0xC0    // the cart header part, not checked (XD changes it)

struct XdSite
{
    u32 addr;   // a BL instruction in the decompressed program
    u32 call;   // XD_CALL_*
};

// One XD link program, as sent by the GameCube and after decompression.
struct XdProgram
{
    u32 recvHash;       // hash of the compressed program, then the loader, as received
    u32 loaderOffset;
    u32 blobStart;      // compressed program, offsets in the received image
    u32 blobEnd;
    u32 decSize;        // size after decompression
    u32 decHash;        // hash of [XD_HEADER_SIZE, decSize) after decompression
    u32 numSites;
    struct XdSite sites[XD_MAX_SITES];
    struct XdParams params;
};

extern const struct XdProgram gXdPrograms[];
extern const u32 gNumXdPrograms;

#endif // GUARD_XD_LINK_H
