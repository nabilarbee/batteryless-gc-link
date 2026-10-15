#ifndef GUARD_REPRO_HW_H
#define GUARD_REPRO_HW_H

// Cart bus access used by the repro backend. On the host (REPRO_HOST_TEST)
// these go to a simulated cart so the write logic can be tested.

#include "repro_types.h"

#define REPRO_SRAM_BANK_SIZE  0x10000
#define REPRO_SRAM_START      0x0E000000
#define REPRO_BANK_REG        0x09000000

#ifdef REPRO_HOST_TEST

u16 HostRomRead16(u32 offset);
void HostRomWrite16(u32 offset, u16 value);
u8 HostSramRead8(u32 offset);
void HostSramWrite8(u32 offset, u8 value);
void HostBankWrite8(u8 value);

#define ROM_READ16(offset)          HostRomRead16(offset)
#define ROM_WRITE16(offset, value)  HostRomWrite16((offset), (value))
#define SRAM_READ8(offset)          HostSramRead8(offset)
#define SRAM_WRITE8(offset, value)  HostSramWrite8((offset), (value))
#define BANK_WRITE8(value)          HostBankWrite8(value)
#define IRQ_DISABLE(saved)          ((saved) = 0)
#define IRQ_RESTORE(saved)          ((void)(saved))

#else

#include "gba/io_reg.h"

#define ROM_READ16(offset)          (*(vu16 *)(0x08000000 + (offset)))
#define ROM_WRITE16(offset, value)  (*(vu16 *)(0x08000000 + (offset)) = (value))
#define SRAM_READ8(offset)          (*(vu8 *)(REPRO_SRAM_START + (offset)))
#define SRAM_WRITE8(offset, value)  (*(vu8 *)(REPRO_SRAM_START + (offset)) = (value))
#define BANK_WRITE8(value)          (*(vu8 *)REPRO_BANK_REG = (value))
#define IRQ_DISABLE(saved)          do { (saved) = REG_IME; REG_IME = 0; } while (0)
#define IRQ_RESTORE(saved)          (REG_IME = (saved))

#endif

#endif // GUARD_REPRO_HW_H
