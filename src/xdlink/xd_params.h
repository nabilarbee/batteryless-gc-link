#ifndef GUARD_XD_PARAMS_H
#define GUARD_XD_PARAMS_H

#include "repro_types.h"

// Addresses inside one of Pokemon XD's GBA link programs (after it has
// decompressed itself), found by analyze_xd.py. The link hook copies the
// matching set into the shim before the program starts.
struct XdParams
{
    // XD's own functions, Thumb (bit 0 set)
    u32 identifyFlash;
    u32 readFlash;
    u32 verifyFlashSector;
    u32 verifyFlashSectorNBytes;
    u32 savePhase;              // 0 when the program never writes the save
    u32 soundVSyncOff;          // m4aSoundVSyncOff
    u32 soundVSyncOn;           // m4aSoundVSyncOn
    // XD's flash library variables
    u32 programFlashByte;
    u32 programFlashSector;
    u32 eraseFlashChip;
    u32 eraseFlashSector;
    u32 waitForFlashWrite;
    u32 flashMaxTime;
    u32 flash;
};

// Which shim entry a redirected call goes to.
enum
{
    XD_CALL_IDENTIFY,
    XD_CALL_READ,
    XD_CALL_VERIFY,
    XD_CALL_VERIFY_N,
    XD_CALL_SAVE_PHASE,
    XD_CALL_COUNT
};

extern struct XdParams gXdParams;

#endif // GUARD_XD_PARAMS_H
