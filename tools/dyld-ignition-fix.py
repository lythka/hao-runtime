#!/usr/bin/env python3
"""dyld-ignition-fix.py DYLD [OUT]: fix x86_64 dyld's PID-1 boot-spec stage count.

macOS 27.x x86_64 dyld carries libignition (dyld's PID-1 bootstrap). Its
__boot_spec_local (a name, then a fixed array of stage pointers, then the stage
count) lists N stages but stores N+1: the x86 table gained graft_mkdir and
rosetta, and goodbye fell out of the array while the count kept it. After the
last stage, _stage_next returns the count word as a stage pointer and
_stage_fire faults: PID 1 SIGSEGV, "initproc failed to start" (27.0
libignition-64~19252 and 27.2 libignition-64~29901). The
arm64e table is consistent. Only an x86_64 PID 1 runs this table, which no
Apple hardware does on 27.

The table is found by symbol (LC_SYMTAB), the stage pointers are verified to
target __stage_* symbols, and the count is set to N in the x86_64 slice only.
That slice is re-signed ad hoc with its own identifier; every other slice is
copied byte for byte. Exit 0 and write nothing if the count is already N
("not affected"); exit 1 if the layout is not recognised.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hao_fat  # noqa: E402

LC_SEGMENT_64, LC_SYMTAB = 0x19, 0x2


def segments_and_symbols(mo):
    segs, syms = [], {}
    ncmds, off = struct.unpack_from("<I", mo, 16)[0], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", mo, off)
        if cmd == LC_SEGMENT_64:
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", mo, off + 24)
            segs.append((vmaddr, vmsize, fileoff, filesize))
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _ = struct.unpack_from("<IIII", mo, off + 8)
            for k in range(nsyms):
                strx, ntype, _, _, value = struct.unpack_from("<IBBHQ", mo, symoff + 16 * k)
                if ntype & 0x0E == 0x0E:  # N_SECT
                    name = mo[stroff + strx:mo.index(b"\0", stroff + strx)].decode(errors="replace")
                    syms[name] = value
        off += size
    return segs, syms


def file_offset(segs, vmaddr):
    for va, vs, fo, fs in segs:
        if va <= vmaddr < va + min(vs, fs):
            return fo + vmaddr - va
    raise ValueError(f"{vmaddr:#x} not in a file-backed segment")


def analyse(thin):
    """(count_offset, stages, count) for a thin x86_64 dyld."""
    segs, syms = segments_and_symbols(thin)
    if "__boot_spec_local" not in syms:
        raise ValueError("no __boot_spec_local symbol")
    stage_names = {v: k for k, v in syms.items() if k.startswith("__stage_")}
    base = file_offset(segs, syms["__boot_spec_local"]) + 0x18
    stages = []
    while len(stages) < 64:
        word = struct.unpack_from("<Q", thin, base + 8 * len(stages))[0]
        if word >> 63:          # chained-fixup bind: not a stage pointer
            break
        target = word & 0xFFFFFFFFF
        if target not in stage_names:
            break
        stages.append(stage_names[target])
    if not stages:
        raise ValueError("no stage pointers after __boot_spec_local")
    count_off = base + 8 * len(stages)
    return count_off, stages, struct.unpack_from("<Q", thin, count_off)[0]


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    data = open(src, "rb").read()
    x86 = [p for c, _, p in hao_fat.slices(data) or [] if c == hao_fat.CPU_X86_64]
    if len(x86) != 1:
        sys.exit(f"{src}: no single x86_64 slice")
    thin = bytearray(x86[0])
    try:
        count_off, stages, count = analyse(thin)
    except (ValueError, struct.error, IndexError) as e:
        sys.exit(f"{src}: layout not recognised: {e}")
    print(f"{src}: {len(stages)} stages ({', '.join(s[8:] for s in stages)}), count {count}")
    if count == len(stages):
        print("not affected")
        return 0
    if count != len(stages) + 1:
        sys.exit(f"unexpected count {count} for {len(stages)} stages")
    struct.pack_into("<Q", thin, count_off, len(stages))
    if not out:
        print("affected (no OUT given)")
        return 0
    with tempfile.TemporaryDirectory() as work:
        path = os.path.join(work, "dyld-x86_64")
        with open(path, "wb") as f:
            f.write(thin)
        ident = "com.apple.darwin.ignition"
        r = subprocess.run(["codesign", "-dv", path], capture_output=True, text=True)
        for line in (r.stderr + r.stdout).splitlines():
            if line.startswith("Identifier="):
                ident = line.split("=", 1)[1].strip()
        subprocess.run(["codesign", "-s", "-", "-f", "-i", ident, path], check=True, capture_output=True)
        signed = open(path, "rb").read()
    fixed = hao_fat.replace_slice(data, hao_fat.CPU_X86_64, signed)
    with open(out, "wb") as f:
        f.write(fixed)
    os.chmod(out, 0o755)
    print(f"{out}: count {count} -> {len(stages)} in the x86_64 slice; sha256 {hashlib.sha256(fixed).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
