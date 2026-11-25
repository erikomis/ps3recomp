#include "cellOvis.h"
#include <stdio.h>
#include "../../runtime/ppu/ppu_memory.h"

#define SPU_ELF_MACHINE 23
#define PT_LOAD_TYPE    1

static int spu_overlay_segments(u32 elf, u32* overlays)
{
    if (!elf || vm_read32(elf) != 0x7F454C46u)
        return 0;
    if ((vm_read32(elf + 4) >> 16) != 0x0102u || vm_read16(elf + 0x12) != SPU_ELF_MACHINE)
        return 0;
    const u32 phoff = vm_read32(elf + 0x1C);
    const u32 phentsize = vm_read16(elf + 0x2A);
    const u32 phnum = vm_read16(elf + 0x2C);
    if (phentsize < 0x20 || phnum > 64)
        return 0;
    u32 n = 0;
    for (u32 i = 0; i < phnum; i++) {
        const u32 a = elf + phoff + i * phentsize;
        if (vm_read32(a) != PT_LOAD_TYPE)
            continue;
        const u32 va = vm_read32(a + 8), sa = vm_read32(a + 0x14);
        for (u32 j = 0; j < phnum; j++) {
            const u32 b = elf + phoff + j * phentsize;
            if (j == i || vm_read32(b) != PT_LOAD_TYPE)
                continue;
            const u32 vb = vm_read32(b + 8), sb = vm_read32(b + 0x14);
            if (sa && sb && va < vb + sb && vb < va + sa) {
                n++;
                break;
            }
        }
    }
    *overlays = n;
    return 1;
}

s32 cellOvisGetOverlayTableSize(u32 elf)
{
    u32 overlays = 0;
    if (!spu_overlay_segments(elf, &overlays)) {
        printf("[cellOvis] GetOverlayTableSize(elf=0x%08X) -> INVAL (not an SPU ELF)\n", elf);
        return (s32)CELL_OVIS_ERROR_INVAL;
    }
    if (overlays) {
        printf("[cellOvis] GetOverlayTableSize(elf=0x%08X): %u overlay segment(s); overlay tables are not implemented\n",
               elf, overlays);
        return (s32)CELL_OVIS_ERROR_INVAL;
    }
    printf("[cellOvis] GetOverlayTableSize(elf=0x%08X) -> 0 (no overlay segments)\n", elf);
    return 0;
}

s32 cellOvisInitializeOverlayTable(u32 ea_ovly_table, u32 elf)
{
    u32 overlays = 0;
    if (!spu_overlay_segments(elf, &overlays)) {
        printf("[cellOvis] InitializeOverlayTable(table=0x%08X, elf=0x%08X) -> INVAL (not an SPU ELF)\n",
               ea_ovly_table, elf);
        return (s32)CELL_OVIS_ERROR_INVAL;
    }
    if (overlays) {
        printf("[cellOvis] InitializeOverlayTable(table=0x%08X, elf=0x%08X): %u overlay segment(s); overlay tables are not implemented\n",
               ea_ovly_table, elf, overlays);
        return (s32)CELL_OVIS_ERROR_INVAL;
    }
    return CELL_OK;
}
