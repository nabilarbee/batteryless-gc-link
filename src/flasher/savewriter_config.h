#ifndef GUARD_SAVEWRITER_CONFIG_H
#define GUARD_SAVEWRITER_CONFIG_H

// Data that make_save_writer.py appends to savewriter_code.bin, at the first
// 32-byte boundary after the code: this struct, then the save itself
// (saveSize bytes). Hashes and HASH_CHUNK are as in config.h.

#define SAVEWRITER_MAGIC   0x57564153 // "SAVW"
#define SAVEWRITER_VERSION 1
#define SAVEWRITER_ID_RANGES 6

struct SaveWriterConfig
{
    // 0x00
    u32 magic;
    u32 version;
    u32 totalSize;      // bytes from the start of this struct, a multiple of 32
    u32 bodyHash;       // hash of bytes [0x20, totalSize)
    u32 pad[4];
    // 0x20
    u32 gameCode;       // cart header game code at 0xAC ("BPEE")
    u32 saveAddr;       // ROM offset of the save area
    u32 saveSize;       // 0x20000
    u32 areaStart;      // the save area rounded out to 256 KiB: what may be
    u32 areaEnd;        // erased. Outside the save it is 0xFF in the dump.
    u32 testAddr;       // erase test block, as in config.h
    u32 numIdRanges;
    u32 idHash;
    // 0x40
    u32 idRanges[SAVEWRITER_ID_RANGES][2];
    u32 pad2[4];
    // 0x80: the save, saveSize bytes
};

#endif // GUARD_SAVEWRITER_CONFIG_H
