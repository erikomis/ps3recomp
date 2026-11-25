#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def lwmutex_block(src):
    start = src.index('#define LWM_OWNER')
    m = re.search(r'static void sys_lwcond_wait\(ppu_context\* ctx\)\s*\{', src)
    assert m, 'sys_lwcond_wait not found'
    i, depth = m.end(), 1
    while depth:
        depth += {'{': 1, '}': -1}.get(src[i], 0)
        i += 1
    return src[start:i]


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "win32_compat.h"
alignas(64) static uint8_t mem[0x10000];
extern "C" uint8_t* vm_base = mem;
extern "C" void vm_write32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; i++) mem[a + i] = (uint8_t)(v >> (24 - 8 * i)); }
extern "C" uint32_t vm_read32(uint64_t a) { return (uint32_t)mem[a] << 24 | mem[a + 1] << 16 | mem[a + 2] << 8 | mem[a + 3]; }
extern "C" int32_t ppu_thread_priority(uint64_t) { return 1000; }
struct ppu_context { uint64_t gpr[32]; uint64_t thread_id; };
%s
extern "C" void ppu_thread_note_block(uint64_t) {}
/* Stubs for the LWMUTEX_TRACE instrumentation the block carries. */
extern "C" volatile long long g_gcm_ref_pub_count = 0;
extern "C" unsigned long long ppu_thread_cpu_us(unsigned) { return 0; }
extern "C" unsigned ppu_thread_prof_pc(unsigned) { return 0; }
int main() {
    const uint32_t LWC = 0x1000, MTX = 0x2468, ATTR = 0x3000;
    memset(mem, 0xAA, sizeof mem);
    vm_write32(ATTR + 0, 0x2);
    vm_write32(ATTR + 4, 0x10);
    ppu_context c = {};
    c.gpr[3] = MTX; c.gpr[4] = ATTR; c.thread_id = 1;
    sys_lwmutex_create(&c);
    c = ppu_context{}; c.gpr[3] = LWC; c.gpr[4] = MTX; c.gpr[5] = 0x3000;
    sys_lwcond_create(&c);
    int fails = 0;
    uint32_t w0 = vm_read32(LWC), w4 = vm_read32(LWC + 4);
    int ok = w0 == MTX && w4 != 0 && w4 != 0xAAAAAAAAu && c.gpr[3] == 0;
    printf("[%%s] create: +0x00 = 0x%%08X (lwmutex 0x%%08X), +0x04 = 0x%%08X (queue id)\n",
           ok ? "PASS" : "FAIL", w0, MTX, w4);
    fails += !ok;
    int guard = 1;
    for (int i = 8; i < 16; i++) guard &= mem[LWC + i] == 0xAA;
    printf("[%%s] create: nothing written past the 8-byte sys_lwcond_t\n", guard ? "PASS" : "FAIL");
    fails += !guard;

    c = ppu_context{}; c.gpr[3] = MTX; c.thread_id = 7;
    sys_lwmutex_trylock(&c);
    c = ppu_context{}; c.gpr[3] = MTX; c.thread_id = 7;
    sys_lwmutex_trylock(&c);
    c = ppu_context{}; c.gpr[3] = LWC; c.gpr[4] = 1000; c.thread_id = 7;
    sys_lwcond_wait(&c);
    /* This tree's sys_lwmutex_trylock is recursive: the two trylocks above left
     * recur == 2, so a faithful wait restores owner 7 / recur 2 (the fork's
     * check expected recur 1 against its own non-recursive trylock). */
    ok = (uint32_t)c.gpr[3] == 0x8001000Bu && vm_read32(MTX + LWM_OWNER) == 7 && vm_read32(MTX + LWM_RECUR) == 2;
    printf("[%%s] wait: times out on lwmutex 0x%%08X and retakes it (ret 0x%%08X, owner 0x%%X, recur %%u)\n",
           ok ? "PASS" : "FAIL", MTX, (uint32_t)c.gpr[3], vm_read32(MTX + LWM_OWNER), vm_read32(MTX + LWM_RECUR));
    fails += !ok;
    return fails ? 1 : 0;
}
'''


def build(tmp, code):
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    cxx = os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
    if not cc or not cxx:
        return None
    c = Path(tmp) / 't.cpp'
    obj = Path(tmp) / 'win32_compat.o'
    exe = Path(tmp) / 't'
    c.write_text(code)
    platform = ROOT / 'runtime/platform'
    libs = ['-lsynchronization'] if os.name == 'nt' else ['-lpthread']
    for cmd in ([cc, '-std=gnu17', '-O1', '-w', '-c', str(platform / 'win32_compat.c'), '-o', str(obj)],
                [cxx, '-std=c++17', '-O1', '-w', '-I' + str(platform), str(c), str(obj), '-o', str(exe), *libs]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-2000:])
            raise SystemExit('FAIL: harness did not compile')
    return exe


def main():
    src = (ROOT / 'runtime/ppu/ppu_sysprx.cpp').read_text()
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, HARNESS % lwmutex_block(src))
        if not exe:
            print('SKIP: no C/C++ compiler')
            return
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end='')
        if r.returncode:
            raise SystemExit('FAIL: sys_lwcond_t layout')
    print('PASS: sys_lwcond_t is +0x00 lwmutex EA, +0x04 queue id')


if __name__ == '__main__':
    main()
