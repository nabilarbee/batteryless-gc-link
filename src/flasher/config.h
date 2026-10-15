#ifndef GUARD_FLASHER_CONFIG_H
#define GUARD_FLASHER_CONFIG_H

// Data that make_flasher.py appends to flasher_code.bin, at the first
// 32-byte boundary after the code. Everything here comes from the user's own
// ROM dump and the repro patches; the code itself carries no ROM data.
//
// The flasher writes up to MAX_SPANS spans of new bytes. For each span, at
// tablesOffset (from the start of this struct):
//   u32 targetHash[n]               hash of each 4 KiB chunk of [hashStart, hashEnd) once written
//   u32 sourceHash[numSources][n]   the same for each state the area may be in before
//                                   (the dump; the dump with an earlier flash applied)
// where n = (hashEnd - hashStart) / HASH_CHUNK, and at bytesOffset the new
// bytes for [overlayStart, overlayEnd).
//
// Hashes are FNV-1a over little-endian 32-bit words: h = (h ^ w) * 0x01000193,
// starting from HASH_BASIS (see hash.s and make_flasher.py).

#define FLASHER_MAGIC    0x48534C46 // "FLSH"
#define FLASHER_VERSION  2
#define HASH_CHUNK       0x1000
#define HASH_BASIS       0x811C9DC5
#define MAX_ID_RANGES    8
#define MAX_SPANS        4
#define MAX_SOURCES      3

struct FlasherSpan
{
    char name[16];      // shown on screen
    u32 overlayStart;   // ROM offsets of the bytes to write
    u32 overlayEnd;
    u32 hashStart;      // the overlay rounded out to 256 KiB
    u32 hashEnd;
    u32 bytesOffset;
    u32 tablesOffset;
    u32 numSources;
    u32 pad;
};

struct FlasherConfig
{
    // 0x00
    u32 magic;
    u32 version;
    u32 totalSize;      // bytes from the start of this struct, a multiple of 32
    u32 bodyHash;       // hash of bytes [0x20, totalSize)
    u32 pad[4];
    // 0x20
    u32 gameCode;       // cart header game code at 0xAC ("BPEE")
    u32 saveAddr;       // ROM offset of the batteryless save area (never written)
    u32 testAddr;       // 512 KiB aligned; [testAddr - 512K, testAddr + 512K) is 0xFF in the dump
    u32 stagingAddr;    // 256 KiB aligned; [stagingAddr, +256K) is 0xFF in the dump
    u32 numIdRanges;
    u32 idHash;         // hash over all idRanges in order
    u32 numSpans;
    u32 pad2;
    // 0x40
    u32 idRanges[MAX_ID_RANGES][2]; // [start, end), 32-byte aligned ROM offsets
    // 0x80
    struct FlasherSpan spans[MAX_SPANS];
    // 0x140
};

#endif // GUARD_FLASHER_CONFIG_H
