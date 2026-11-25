/* v0.67: the PC stand-in for <ppu-types.h>, with enough types for PSL1GHT's REAL rsx / gcm / video headers
 * to compile on the PC (host/fake_rsx.c, host/rsx_host.sh): the structures and constants gfx.c uses are
 * then the toolchain's own, not copies. */
#ifndef PPU_TYPES_STUB
#define PPU_TYPES_STUB
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef volatile u8 vu8; typedef volatile u16 vu16; typedef volatile u32 vu32; typedef volatile u64 vu64;
typedef volatile s8 vs8; typedef volatile s16 vs16; typedef volatile s32 vs32; typedef volatile s64 vs64;
typedef float f32; typedef double f64;
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
#endif
