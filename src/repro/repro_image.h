#ifndef GUARD_REPRO_IMAGE_H
#define GUARD_REPRO_IMAGE_H

// 128 KiB working copy of the save in the cart's banked SRAM, and the code
// that commits it to the save area in ROM.

#include "repro_types.h"
#include "repro_cart.h"

u16 ReproImage_Load(u32 saveOffset);
void ReproImage_Read(u32 offset, u8 *dest, u32 size);
u32 ReproImage_Compare(u32 offset, const u8 *src, u32 size);
void ReproImage_Write(u32 offset, const u8 *src, u32 size);
void ReproImage_Fill(u32 offset, u8 value, u32 size);
u16 ReproImage_Commit(const struct ReproCart *cart, u32 saveOffset);

#endif // GUARD_REPRO_IMAGE_H
