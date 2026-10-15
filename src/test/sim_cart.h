// Simulated bootleg batteryless cart for host tests: ROM flash chip(s) with
// AMD or Intel command sets, optional D0/D1 swap, and 128 KiB banked SRAM.
#ifndef SIM_CART_H
#define SIM_CART_H

#include <stdint.h>

#define SIM_ROM_SIZE  (32 * 1024 * 1024)
#define SIM_SRAM_SIZE (128 * 1024)

enum SimBoard
{
    SIM_PLAIN_ROM,       // mask ROM: ignores every write
    SIM_INTEL_BUFFERED,  // Intel, MLC-style: 1 KiB regions programmed once, buffered only
    SIM_INTEL_WORD,      // Intel, word programming
    SIM_AMD_SWAPPED,     // AMD x16, D0/D1 swapped
    SIM_AMD_DUAL,        // two AMD x8 chips, low chip D0/D1 swapped
    SIM_AMD_X16,         // AMD x16 that answers CFI on both lanes
    SIM_INTEL_UNKNOWN,   // Intel command set, device ID not in any list
};

struct SimStats
{
    unsigned erases;
    unsigned programs;        // word or buffer program operations
    unsigned violations;      // reprogramming, programming locked blocks, bad commands
    unsigned strayWrites;     // writes ignored in read mode
    unsigned badBankWrites;   // bank register values this board does not use
};

void SimInit(enum SimBoard board, uint32_t seed);
uint8_t *SimRom(void);        // array contents as seen by the GBA in read mode
uint8_t *SimSram(void);       // 128 KiB, bank 0 then bank 1
struct SimStats *SimStats(void);
void SimSetEraseBlock(uint32_t size);
void SimFailNextErase(void);
void SimFailNextProgram(void);
const char *SimLastViolation(void);
void SimSetCfiBlock(uint32_t size);   // make the CFI table report another block size
void SimPowerCycle(void);
uint32_t SimLastErase(void);   // start of the block erased last
uint32_t SimEraseBlock(void);

#endif
