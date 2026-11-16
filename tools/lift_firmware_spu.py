#!/usr/bin/env python3
import argparse
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build_spu_workloads import debase_includes
from spu_disasm import disassemble_spu
from spu_elf import scan_spu_candidates

EM_SPU = 23


def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]


def spu_elfs(img):
    out, off = [], img.find(b"\x7FELF")
    while off != -1:
        b = img[off:]
        if len(b) >= 0x34 and b[4] == 1 and b[5] == 2 and be16(b, 18) == EM_SPU:
            phoff, shoff = be32(b, 0x1C), be32(b, 0x20)
            phnum, shnum, shent = be16(b, 0x2C), be16(b, 0x30), be16(b, 0x2E)
            end = shoff + shnum * shent
            for i in range(phnum):
                p = phoff + i * 32
                end = max(end, p + 32, be32(b, p + 4) + be32(b, p + 16))
            out.append((off, end, be32(b, 0x18)))
            off = img.find(b"\x7FELF", off + end)
        else:
            off = img.find(b"\x7FELF", off + 1)
    return out


def load_segments(elf):
    phoff, phnum = be32(elf, 0x1C), be16(elf, 0x2C)
    segs = []
    for i in range(phnum):
        p = phoff + i * 32
        if be32(elf, p) == 1:
            segs.append((be32(elf, p + 4), be32(elf, p + 8), be32(elf, p + 16)))
    return segs


def code_pointers(elf):
    out = set()
    segs = load_segments(elf)
    for off, va, fs in segs:
        for k in range(0, fs - 3, 4):
            w = be32(elf, off + k)
            if w >= 0x200 and w % 4 == 0 and va <= w < va + fs and be32(elf, off + (w - va)):
                out.add(w)
    return sorted(out)


def stack_sets(code, ls_base):
    out = []
    for i in disassemble_spu(code, base_addr=ls_base):
        if i.mnemonic in ("il", "ila", "lqa") and re.match(r"\$r1,", i.operands):
            out.append(i.addr)
    return out


def lifted_entries(src):
    return {int(m.group(1), 16) for m in re.finditer(r"spu_func_([0-9A-F]{8})\(spu_context\* ctx\) \{", open(src).read())}


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("failed: " + " ".join(cmd[:3]))
    return r.stdout + r.stderr


def lift(cmd, d):
    report = os.path.join(d, "lift.json")
    log = run(cmd + ["--report", report])
    open(os.path.join(d, "lift.log"), "w").write(log)
    diag = json.load(open(report))
    if diag.get("unsupported"):
        raise SystemExit("%s: unsupported instructions %s" % (d, diag["unsupported"]))
    src = os.path.join(d, "spu_recomp.c")
    for f in ("spu_recomp.c", "spu_recomp.h"):
        debase_includes(os.path.join(d, f))
    if "/* TODO spu:" in open(src).read():
        raise SystemExit("%s: unresolved instruction or branch" % d)
    return src, len(diag.get("functions", []))


def main():
    ap = argparse.ArgumentParser(
        description="Lift the SPU side of a relocated firmware module: its embedded SPU ELF "
                    "images, and the code bodies it streams into local store.")
    ap.add_argument("--image", help="relocated module image (lift_prx.py <name>_image.bin)")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0, help="guest EA the image is loaded at")
    ap.add_argument("--name", required=True, help="symbol prefix stem, e.g. libsre")
    ap.add_argument("--first-image-id", type=int, default=100)
    ap.add_argument("--body", action="append", default=[],
                    help="OFF:SIZE:LSA:ENTRY:CODELEN (hex) of a code body in the image: lifted at LSA, "
                         "code bounded by CODELEN, registered as a code region at that address")
    ap.add_argument("--pic", action="append", default=[],
                    help="a position-independent firmware SPU ELF (dev_flash .pic plugin) that a title loads "
                         "at a run-time LS address: lifted relocatable at its link address and registered "
                         "by its code bytes")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    img = open(a.image, "rb").read() if a.image else b""
    os.makedirs(a.out, exist_ok=True)
    py = sys.executable
    lifter = os.path.join(HERE, "spu_lifter.py")
    regs = []
    sources = []
    image_id = a.first_image_id

    for off, size, entry in spu_elfs(img):
        name = "%s_spu_%05X" % (a.name, off)
        d = os.path.join(a.out, name)
        os.makedirs(d, exist_ok=True)
        elf_bytes = img[off:off + size]
        elf = os.path.join(d, "image.elf")
        open(elf, "wb").write(elf_bytes)
        p = be32(elf_bytes, 0x1C)
        seg = elf_bytes[be32(elf_bytes, p + 4):be32(elf_bytes, p + 4) + be32(elf_bytes, p + 16)]
        sets = stack_sets(seg, be32(elf_bytes, p + 8))
        seeds = sorted(set(code_pointers(elf_bytes)) | set(sets))
        cmd = [py, lifter, "--auto-functions", elf, "--symbol-prefix", name + "_", "--merge-chunks",
               "--return-entries", "-o", d, "--extra-funcs", ",".join("0x%X" % x for x in seeds)]
        src, n = lift(cmd, d)
        resets = sorted(set(sets) & lifted_entries(src))
        print("%s: SPU ELF at +0x%X (EA 0x%08X) entry 0x%X, %d functions, image %d, seeds %s, stack resets %s"
              % (name, off, a.base + off, entry, n, image_id, " ".join("0x%X" % x for x in seeds) or "-",
                 " ".join("0x%X" % x for x in resets) or "-"))
        regs.append((name, image_id, None, resets, None))
        sources.append(src)
        image_id += 1

    for spec in a.body:
        off, size, lsa, entry, codelen = (int(x, 16) for x in spec.split(":"))
        if codelen > size:
            raise SystemExit("body +0x%X: CODELEN 0x%X exceeds SIZE 0x%X" % (off, codelen, size))
        name = "%s_body_%05X" % (a.name, off)
        d = os.path.join(a.out, name)
        os.makedirs(d, exist_ok=True)
        raw = os.path.join(d, "body.bin")
        open(raw, "wb").write(img[off:off + size])
        elf = os.path.join(d, "image.elf")
        run([py, os.path.join(HERE, "wrap_spu_elf.py"), raw, "--entry", hex(entry), "--base", hex(lsa), "--out", elf])
        sets = stack_sets(img[off:off + codelen], lsa)
        cmd = [py, lifter, "--auto-functions", elf, "--base", hex(lsa), "--symbol-prefix", name + "_",
               "--code-end", hex(lsa + codelen), "--merge-chunks", "--return-entries", "-o", d]
        if sets:
            cmd += ["--extra-funcs", ",".join("0x%X" % x for x in sets)]
        src, n = lift(cmd, d)
        resets = sorted(set(sets) & lifted_entries(src))
        print("%s: body at +0x%X (EA 0x%08X) code 0x%X of 0x%X -> LS 0x%X, %d functions, image %d, stack resets %s"
              % (name, off, a.base + off, codelen, size, lsa, n, image_id, " ".join("0x%X" % x for x in resets) or "-"))
        regs.append((name, image_id, (a.base + off, lsa, codelen), resets, None))
        sources.append(src)
        image_id += 1

    for path in a.pic:
        elf_bytes = open(path, "rb").read()
        stem = re.sub(r"[^A-Za-z0-9_]", "_", os.path.splitext(os.path.basename(path))[0])
        name = "%s_pic_%s" % (a.name, stem)
        d = os.path.join(a.out, name)
        os.makedirs(d, exist_ok=True)
        elf = os.path.join(d, "image.elf")
        open(elf, "wb").write(elf_bytes)
        info = next((i for o, i, e in scan_spu_candidates(elf_bytes) if o == 0 and not e), None)
        if not info or len(info["executable"]) != 1:
            raise SystemExit("%s: not a single-code-segment SPU ELF" % path)
        x = info["executable"][0]
        text = [s["addr"] + s["size"] for s in info["sections"]
                if s["flags"] & 4 and s["size"] and x["addr"] <= s["addr"] < x["addr"] + x["filesz"]]
        code_end = max(text) if text else x["addr"] + x["filesz"]
        code = elf_bytes[x["off"]:x["off"] + (code_end - x["addr"])]
        src, n = lift([py, lifter, "--auto-functions", elf, "--reloc", "--symbol-prefix", name + "_",
                       "--code-end", hex(code_end), "--merge-chunks", "-o", d], d)
        print("%s: %s code 0x%X at link 0x%X, %d functions, image %d"
              % (name, os.path.basename(path), len(code), x["addr"], n, image_id))
        regs.append((name, image_id, None, [], (code, x["addr"])))
        sources.append(src)
        image_id += 1

    lines = ["#include <stdint.h>", "",
             "void spu_begin_image(int image_id);",
             "void spu_overlay_register_region_at(uint32_t content_ea, uint32_t lsa, uint32_t span, int image_id);",
             "int spu_overlay_register_reloc_content(const uint8_t* code, uint32_t span, uint32_t canon, int image_id);",
             "void spu_register_stack_reset_entry(uint32_t entry, int image_id);"]
    lines += ["void %s_spu_recomp_register(void);" % name for name, _, _, _, _ in regs]
    for name, _, _, _, content in regs:
        if content:
            lines += ["", "static const uint8_t %s_code[0x%X] = {" % (name, len(content[0]))]
            lines += ["    " + "".join("0x%02X," % c for c in content[0][k:k + 24]) for k in range(0, len(content[0]), 24)]
            lines += ["};"]
    lines += ["", "void %s_spu_register(void)" % a.name, "{"]
    for name, iid, region, resets, content in regs:
        lines.append("    spu_begin_image(%d);" % iid)
        lines.append("    %s_spu_recomp_register();" % name)
        if region:
            lines.append("    spu_overlay_register_region_at(0x%08Xu, 0x%Xu, 0x%Xu, %d);" % (region[0], region[1], region[2], iid))
        if content:
            lines.append("    spu_overlay_register_reloc_content(%s_code, 0x%Xu, 0x%Xu, %d);" % (name, len(content[0]), content[1], iid))
        for r in resets:
            lines.append("    spu_register_stack_reset_entry(0x%Xu, %d);" % (r, iid))
    lines += ["    spu_begin_image(0);", "}", ""]
    reg = os.path.join(a.out, "%s_spu_register.c" % a.name)
    open(reg, "w").write("\n".join(lines))
    sources.append(reg)
    with open(os.path.join(a.out, "sources.cmake"), "w") as f:
        f.write("set(FIRMWARE_SPU_SOURCES\n" + "".join("  [==[%s]==]\n" % os.path.abspath(s).replace("\\", "/") for s in sources) + ")\n")
    print("wrote", reg)


if __name__ == "__main__":
    main()
