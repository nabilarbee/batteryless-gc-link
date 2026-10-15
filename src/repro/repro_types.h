#ifndef GUARD_REPRO_TYPES_H
#define GUARD_REPRO_TYPES_H

// Lets the hardware-independent repro code build on the host for tests.
#ifdef REPRO_HOST_TEST
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef volatile uint8_t vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
typedef u8 bool8;
typedef u32 bool32;
#define TRUE 1
#define FALSE 0
#else
#include "gba/types.h"
#include "gba/defines.h"
#endif

#endif // GUARD_REPRO_TYPES_H
