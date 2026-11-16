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
import lift_jobmods as lj
from build_spu_workloads import debase_includes
from find_spu_functions import reachable_code_end
from spu_disasm import disassemble_spu
from spu_elf import scan_spu_candidates
from wrap_spu_elf import wrap

ENTRY_OFFSETS = (0x0, 0x10, 0x18)


def load_segments(path):
    b = open(path, "rb").read()
    if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 2:
        raise SystemExit("not a big-endian ELF64 PPU image: " + path)
    phoff = struct.unpack(">Q", b[0x20:0x28])[0]
    phnum = struct.unpack(">H", b[0x38:0x3A])[0]
    segs = []
    for i in range(phnum):
        p = phoff + 56 * i
        if struct.unpack(">I", b[p:p + 4])[0] != 1:
            continue
        off, va = struct.unpack(">QQ", b[p + 8:p + 24])
        fsz = struct.unpack(">Q", b[p + 32:p + 40])[0]
        segs.append((va, b[off:off + fsz]))
    return segs


HEADER = re.compile(rb"(?=(?:[\x42\x43]..[\x02\x82]){4})", re.S)


def find_jobs(data):
    starts = [m.start() for m in HEADER.finditer(data) if m.start() % 16 == 0]
    out = []
    for n, s in enumerate(starts):
        if n + 1 < len(starts):
            span = starts[n + 1] - s
        else:
            z = data.find(b"\x00" * 256, s)
            span = ((z if z > 0 else len(data)) - s + 15) & ~15
        out.append((s, span))
    return out


OPERAND = re.compile(r"\$r(\d+), 0x([0-9A-F]+)$")


def spu_code_segments(segs):
    out = {}
    for va, data in segs:
        for off, info, error in scan_spu_candidates(data):
            if error:
                continue
            for x in info["executable"]:
                lo, hi = x["addr"], x["addr"] + x["filesz"]
                text = [s["addr"] + s["size"] for s in info["sections"]
                        if s["flags"] & 4 and s["size"] and lo <= s["addr"] < hi]
                out[va + off + x["off"]] = (lo, max(text) if text else hi)
    return out


def link_base(raw):
    votes = {}
    ins = disassemble_spu(raw, base_addr=0)
    for k, i in enumerate(ins):
        m = OPERAND.match(i.operands) if i.mnemonic == "brsl" else None
        if not m or m.group(1) == "0":
            continue
        link = i.addr + 4
        for j in ins[max(0, k - 4):k + 4]:
            n = OPERAND.match(j.operands) if j.mnemonic == "ila" else None
            if n:
                base = int(n.group(2), 16) - link
                if base > 0 and base % 16 == 0:
                    votes[base] = votes.get(base, 0) + 1
    return max(votes, key=votes.get) if votes else None


def main():
    ap = argparse.ArgumentParser(
        description="Lift the position-independent SPURS job binaries embedded in a PPU ELF "
                    "(four `ila $r2` header words) once each at their link base, and register "
                    "them as relocatable code regions at their guest addresses. Task images "
                    "(SPU ELF code linked at LS 0x3000 and up) are not jobs and are skipped.")
    ap.add_argument("elf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--first-image-id", type=lambda x: int(x, 0), default=0x20000000)
    ap.add_argument("--register-fn", default="spu_jobs_register_all")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    segs = load_segments(a.elf)
    elf_code = spu_code_segments(segs)
    jobs, tasks = [], 0
    for va, data in segs:
        for s, span in find_jobs(data):
            ea = va + s
            if ea in elf_code and elf_code[ea][0] >= 0x3000:
                tasks += 1
                continue
            raw = data[s:s + span]
            if ea in elf_code:
                lo, code_end = elf_code[ea]
                jobs.append([ea, raw[:code_end - lo], lo, "elf"])
            else:
                canon = link_base(raw)
                jobs.append([ea, raw, canon, "idiom" if canon else "default"])
    if not jobs:
        raise SystemExit("no SPURS job binaries found in " + a.elf)
    known = [j[2] for j in jobs if j[3] == "idiom"]
    default = max(set(known), key=known.count) if known else 0x100
    for j in jobs:
        if j[2] is None:
            j[2] = default
    print("%d job headers, %d task images skipped; default raw-job link base 0x%X" % (len(jobs), tasks, default))

    lifter = os.path.join(HERE, "spu_lifter.py")
    regs, sources, report = [], [], []
    for n, (ea, raw, canon, how) in enumerate(jobs):
        raw = raw[:min(len(raw), 0x40000 - canon)]
        name = "spujob_%08X" % ea
        entries = {canon + o for o in ENTRY_OFFSETS}
        entries |= set(lj.manual_link_targets(raw, base=canon))
        if how == "elf":
            span = len(raw)
        else:
            ce, undecodable = reachable_code_end(raw, canon, entries)
            if undecodable:
                raise SystemExit("%s: control flow reaches undecodable words at %s (EA 0x%08X)"
                                 % (name, " ".join("0x%X" % x for x in undecodable[:8]), ea))
            span = ce - canon
            raw = raw[:span]
        d = os.path.join(a.out, name)
        os.makedirs(d, exist_ok=True)
        elf = os.path.join(d, "image.elf")
        with open(elf, "wb") as f:
            f.write(wrap(raw, base=canon, entry=canon))
        extras = set(entries)
        extras |= set(lj.switch_table_targets(raw, base=canon))
        extras = sorted(x for x in extras if canon <= x < canon + span and x % 4 == 0)
        cmd = [sys.executable, lifter, "--auto-functions", elf, "--base", hex(canon), "--reloc",
               "--symbol-prefix", name + "_", "--code-end", hex(canon + span), "--merge-chunks",
               "--extra-funcs", ",".join(hex(x) for x in extras), "--report", os.path.join(d, "lift.json"),
               "-o", d]
        r = subprocess.run(cmd, capture_output=True, text=True)
        open(os.path.join(d, "lift.log"), "w").write(r.stdout + r.stderr)
        if r.returncode:
            raise SystemExit("%s: lift failed, see %s" % (name, os.path.join(d, "lift.log")))
        diag = json.load(open(os.path.join(d, "lift.json")))
        if diag.get("unsupported"):
            raise SystemExit("%s: unsupported instructions %s" % (name, diag["unsupported"]))
        for fn in ("spu_recomp.c", "spu_recomp.h"):
            debase_includes(os.path.join(d, fn))
        if "/* TODO spu:" in open(os.path.join(d, "spu_recomp.c")).read():
            raise SystemExit("%s: unresolved instruction or branch" % name)
        image_id = a.first_image_id + n
        regs.append((name, ea, raw, canon, image_id))
        sources.append(os.path.join(d, "spu_recomp.c"))
        report.append(dict(name=name, ea="0x%08X" % ea, span="0x%X" % span, link_base="0x%X" % canon,
                           link_base_from=how, image_id=image_id, functions=len(diag.get("functions", []))))
        print("%s: EA 0x%08X code 0x%X, link base 0x%X (%s) -> image 0x%X, %d functions"
              % (name, ea, span, canon, how, image_id, len(diag.get("functions", []))))

    lines = ["#include <stdint.h>", "",
             "void spu_begin_image(int image_id);",
             "int spu_overlay_register_reloc_content(const uint8_t* code, uint32_t span, uint32_t canon, int image_id);"]
    lines += ["void %s_spu_recomp_register(void);" % name for name, _, _, _, _ in regs]
    for name, ea, code, canon, image_id in regs:
        lines.append("")
        lines.append("static const uint8_t %s_code[0x%X] = {" % (name, len(code)))
        for k in range(0, len(code), 24):
            lines.append("    " + "".join("0x%02X," % c for c in code[k:k + 24]))
        lines.append("};")
    lines += ["", "void %s(void)" % a.register_fn, "{"]
    for name, ea, code, canon, image_id in regs:
        lines.append("    spu_begin_image(%d);" % image_id)
        lines.append("    %s_spu_recomp_register();" % name)
        lines.append("    spu_overlay_register_reloc_content(%s_code, 0x%Xu, 0x%Xu, %d);" % (name, len(code), canon, image_id))
    lines += ["    spu_begin_image(0);", "}", "",
              "__attribute__((constructor)) static void %s_ctor(void)" % a.register_fn, "{",
              "    %s();" % a.register_fn, "}", ""]
    reg = os.path.join(a.out, "spu_jobs_register.c")
    open(reg, "w").write("\n".join(lines))
    sources.append(reg)
    with open(os.path.join(a.out, "sources.cmake"), "w") as f:
        f.write("set(SPU_JOB_SOURCES\n" + "".join("  [==[%s]==]\n" % os.path.abspath(s).replace("\\", "/") for s in sources) + ")\n")
    json.dump(dict(jobs=report, tasks_skipped=tasks), open(os.path.join(a.out, "inventory.json"), "w"), indent=1)
    print("%d SPURS job binaries registered" % len(regs))


if __name__ == "__main__":
    main()
