#!/usr/bin/env python3
"""stamp_identity.py [--print] KC [HaoExec.kext]: bind HaoExec to one kernel.

Reads the kernel identity HaoExec needs from a BootKernelExtensions.kc (the
com.apple.kernel fileset entry: LC_UUID, its LC_SYMTAB, the activator table
contents, the addresses of the kernel functions the kext calls) and writes it
into the kext's __DATA,__hao_identity section. The kext re-verifies all of it
at load and stays inactive on any mismatch, so a kext stamped for another
kernel does nothing: it reads the kernel header only after every stamped
function gives the same slide. The activator table must be exactly
[mach, fat, shell, NULL], the layout the kext hard-codes (orig_shell =
table[2]); anything else is refused here. Pure Python: runs in the image builder
right after the collection it describes is built. --print shows the values.
Load commands are walked by hao_fat.load_commands (tools/hao_fat.py, which
ships next to this file in a payload); a malformed collection or kext is
refused with a message, not a traceback.
"""
import os
import struct
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here if os.path.isfile(os.path.join(_here, "hao_fat.py")) else os.path.join(_here, "..", "tools"))
import hao_fat  # noqa: E402

MAGIC, VERSION = 0x494F4148, 2          # "HAOI"
LAYOUT = "<II16sQQQQQQQQQ"              # magic, version, uuid, header, printf, table, act[3], slide checks[3]
SLIDE_CHECKS = ("_IOSleep", "_ml_set_interrupts_enabled", "_sysctl_register_oid")
WANT = {"_printf", "_image_activation_callbacks", *SLIDE_CHECKS}
LC_SEGMENT_64, LC_SYMTAB, LC_UUID, LC_FILESET_ENTRY = 0x19, 0x2, 0x1B, 0x80000035


def commands(buf, hoff):
    """(cmd, offset, cmdsize) of the Mach-O at HOFF: bounded, ValueError if malformed."""
    return hao_fat.load_commands(buf, "Mach-O at %#x" % hoff, hoff)


def kernel_identity(kc):
    entry = None
    for cmd, off, size in commands(kc, 0):
        if cmd == LC_FILESET_ENTRY and size >= 32:
            vm, fo, stroff = struct.unpack_from("<QQI", kc, off + 8)
            if kc[off + stroff:off + size].split(b"\0")[0] == b"com.apple.kernel":
                entry = (vm, fo)
    if not entry:
        raise SystemExit("com.apple.kernel not found in the kernel collection")
    header_vm, hoff = entry
    segs, uuid, syms = [], None, {}
    for cmd, off, size in commands(kc, hoff):
        if cmd == LC_SEGMENT_64 and size >= 72:
            vm, vs, fo, fs = struct.unpack_from("<QQQQ", kc, off + 24)
            segs.append((vm, vs, fo, fs))
        elif cmd == LC_UUID and size >= 24:
            uuid = kc[off + 8:off + 24]
        elif cmd == LC_SYMTAB and size >= 24:
            symoff, nsyms, stroff, _ = struct.unpack_from("<IIII", kc, off + 8)
            for k in range(nsyms):
                strx, ntype, _, _, value = struct.unpack_from("<IBBHQ", kc, symoff + 16 * k)
                if ntype & 0x0E == 0x0E:
                    end = kc.index(b"\0", stroff + strx)
                    name = kc[stroff + strx:end]
                    if name.decode(errors="replace") in WANT:
                        syms[name.decode()] = value
    if uuid is None or set(syms) != WANT:
        raise SystemExit(f"kernel entry lacks LC_UUID or symbols (found {sorted(syms)})")

    def read(va, n):
        for vm, vs, fo, fs in segs:
            if vm <= va < vm + min(vs, fs):
                return kc[fo + va - vm:fo + va - vm + n]
        raise SystemExit(f"address {va:#x} not file-backed in the kernel entry")

    table = syms["_image_activation_callbacks"]
    words = struct.unpack("<4Q", read(table, 32))
    acts = words[:3]
    if 0 in acts or words[3] != 0:
        raise SystemExit("activator table is not [mach, fat, shell, NULL] "
                         f"({' '.join(hex(w) for w in words)}); HaoExec hard-codes that layout")
    checks = tuple(syms[s] for s in SLIDE_CHECKS)
    return uuid, header_vm, syms["_printf"], table, acts, checks


def stamp(kext_binary, values):
    data = bytearray(open(kext_binary, "rb").read())
    sect = None
    for cmd, off, size in commands(data, 0):
        if cmd == LC_SEGMENT_64 and size >= 72:
            nsect = min(struct.unpack_from("<I", data, off + 64)[0], (size - 72) // 80)
            for k in range(nsect):
                so = off + 72 + 80 * k
                if data[so:so + 16].split(b"\0")[0] == b"__hao_identity":
                    size_, fo = struct.unpack_from("<QI", data, so + 40)[0], struct.unpack_from("<I", data, so + 48)[0]
                    sect = (fo, size_)
    if not sect or sect[1] < struct.calcsize(LAYOUT):
        raise SystemExit("kext has no __hao_identity section")
    magic, version = struct.unpack_from("<II", data, sect[0])
    if (magic, version) != (MAGIC, VERSION):
        raise SystemExit(f"unexpected identity block {magic:#x}/{version}")
    uuid, header, printf, table, acts, checks = values
    struct.pack_into(LAYOUT, data, sect[0], MAGIC, VERSION, uuid, header, printf, table, *acts, *checks)
    open(kext_binary, "wb").write(data)


def main():
    args = sys.argv[1:]
    show = "--print" in args
    args = [a for a in args if a != "--print"]
    values = kernel_identity(open(args[0], "rb").read())
    uuid, header, printf, table, acts, checks = values
    if show or len(args) == 1:
        print(f"uuid {uuid.hex()} header {header:#x} printf {printf:#x} table {table:#x} "
              f"activators {' '.join(hex(a) for a in acts)} slide checks {' '.join(hex(c) for c in checks)}")
    if len(args) > 1:
        stamp(args[1].rstrip("/") + "/Contents/MacOS/HaoExec", values)
        print(f"stamped {args[1]} for kernel {uuid.hex()}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, struct.error) as e:
        sys.exit("stamp_identity: malformed input: %s" % e)
