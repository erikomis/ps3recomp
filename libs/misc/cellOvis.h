#ifndef PS3RECOMP_CELL_OVIS_H
#define PS3RECOMP_CELL_OVIS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_OVIS_ERROR_INVAL 0x80410402
#define CELL_OVIS_ERROR_ABORT 0x8041040C
#define CELL_OVIS_ERROR_ALIGN 0x80410410

s32 cellOvisGetOverlayTableSize(u32 elf);
s32 cellOvisInitializeOverlayTable(u32 ea_ovly_table, u32 elf);

#ifdef __cplusplus
}
#endif

#endif
