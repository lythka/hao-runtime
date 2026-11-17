#!/usr/bin/env python3
"""One answer from both readers of a guest's code signature.

src/libval.c (hao_codesig_parse, hao_slice_verify) decides at run time what a
guest's signature is; tools/hao_codesig.py (parse, check) reads the same
signature for the packaging tools. This runs both over the fuzz corpus
(tests/fuzz/corpus/codesig, the Mach-O seeds of tests/fuzz/corpus/macho) and
over fixed mutations of each input, and requires the same verdict from both,
and for a valid signature the same cdhash, flags, platform identifier, Team
ID, disable-library-validation and library-validation answer:
  - every byte of the first 400 of a signature blob, changed four ways;
  - in a Mach-O: every byte of the first 256 of its signature changed two
    ways, a changed byte in its first, a middle and its last hashed page, a
    byte appended, the last byte removed, the signature's range moved, a
    second LC_CODE_SIGNATURE, and __TEXT, function starts or the symbol
    table reaching into the signature (which tools/hao_codesig.py's
    unsealed_use must also name by itself).
The C side is build/test_macho --codesig LIST (an ASan build, so a read
outside a signature fails the run).

  test_codesig.py TEST_MACHO [SCRATCH_DIR]
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
import hao_codesig  # noqa: E402
import hao_fat  # noqa: E402

CS_REQUIRE_LV, CS_RUNTIME = 0x2000, 0x10000


def line(verdict, s=None):
    if s is None:
        return verdict
    lv = int(not s["dlv"] and bool(s["platform"] or s["flags"] & (CS_RUNTIME | CS_REQUIRE_LV)))
    return "%s %s flags=0x%x platform=%d team=%s dlv=%d lv=%d" % (
        verdict, s["cdhash"].hex(), s["flags"], s["platform"], (s["team"] or b"").hex(), int(s["dlv"]), lv)


def blob_line(blob):
    try:
        return line("valid", hao_codesig.parse(blob))
    except ValueError:
        return line("invalid")


def arm_slice(data):
    """The slice src/macho.c runs: plain arm64e, else another arm64e, else arm64."""
    best, rank = None, 0
    for cpu, sub, payload in hao_fat.slices(data) or []:
        if cpu != hao_fat.CPU_ARM64:
            continue
        r = (3 if sub & 0xFF000000 in (0, 0x80000000) else 2) if sub & 0xFFFFFF == 2 else 1
        if r > rank:
            best, rank = payload, r
    return best


def macho_line(data):
    mo = arm_slice(data)
    if mo is None:
        return line("refused")
    try:
        c = hao_codesig.check(bytes(mo))
    except ValueError:
        return line("refused")
    return line(c["verdict"], c if c["verdict"] == "valid" else None)


def signature_command(mo):
    """(offset of the LC_CODE_SIGNATURE command, data offset, data size) of a thin Mach-O."""
    for cmd, off, size in hao_fat.load_commands(mo, "Mach-O"):
        if cmd == hao_codesig.LC_CODE_SIGNATURE:
            return (off,) + struct.unpack_from("<II", mo, off + 8)
    return None


def blob_cases(blob):
    yield blob
    for at in range(min(len(blob), 400)):
        for change in (lambda b: b ^ 0x01, lambda b: b ^ 0x80, lambda b: 0x00, lambda b: 0xFF):
            m = bytearray(blob)
            if change(m[at]) != m[at]:
                m[at] = change(m[at])
                yield bytes(m)
    yield blob[:len(blob) // 2]


def unsealed_cases(mo):
    """MO with __TEXT ending 16 bytes into the signature; function starts and the symbol table starting at
    it; __DATA mapped from __LINKEDIT's file range; __LINKEDIT writable; and an initializer offsets section
    added to __LINKEDIT over the signature (src/libval.c unsealed_use)."""
    sig = signature_command(mo)
    cmds = list(hao_fat.load_commands(mo, "Mach-O"))
    seg = {mo[off + 8:off + 24].rstrip(b"\0"): (off, size) for cmd, off, size in cmds if cmd == 0x19}
    for cmd, off, size in cmds:
        m = bytearray(mo)
        if cmd == 0x19 and mo[off + 8:off + 24].rstrip(b"\0") == b"__TEXT":
            struct.pack_into("<Q", m, off + 48, sig[1] + 16 - struct.unpack_from("<Q", mo, off + 40)[0])
        elif cmd in (0x26, 0x2):                        # LC_FUNCTION_STARTS dataoff, LC_SYMTAB symoff
            struct.pack_into("<I", m, off + 8, sig[1])
        else:
            continue
        yield bytes(m)
    if b"__LINKEDIT" not in seg:
        return
    le, lsize = seg[b"__LINKEDIT"]
    if b"__DATA" in seg:
        m = bytearray(mo)
        d = seg[b"__DATA"][0]
        m[d + 40:d + 56] = mo[le + 40:le + 56]          # fileoff, filesize
        yield bytes(m)
    m = bytearray(mo)
    for at in (56, 60):                                 # maxprot, initprot |= write
        struct.pack_into("<i", m, le + at, struct.unpack_from("<i", mo, le + at)[0] | 2)
    yield bytes(m)
    sizeofcmds = struct.unpack_from("<I", mo, 20)[0]
    end = 32 + sizeofcmds
    if end + 80 <= sig[1]:                              # in the 80 bytes after the commands (the code's start)
        vm, _, fo = struct.unpack_from("<QQQ", mo, le + 24)
        sect = struct.pack("<16s16sQQIIIIIIII", b"__init_offsets", b"__LINKEDIT", vm + sig[1] - fo, 4, sig[1], 0, 0, 0,
                           0x16, 0, 0, 0)
        m = bytearray(mo[:le + lsize] + sect + mo[le + lsize:end] + mo[end + 80:])
        struct.pack_into("<I", m, le + 4, lsize + 80)
        struct.pack_into("<I", m, le + 64, struct.unpack_from("<I", mo, le + 64)[0] + 1)
        struct.pack_into("<I", m, 20, sizeofcmds + 80)
        yield bytes(m)


def macho_cases(mo):
    yield mo
    sig = signature_command(mo)
    if not sig:
        return
    cmd, off, size = sig
    for at in range(off, off + min(size, 256)):
        for x in (0x01, 0x80):
            m = bytearray(mo)
            m[at] ^= x
            yield bytes(m)
    for at in (0x40, off // 2, off - 1):                 # a hashed page: the first, a middle one, the last
        m = bytearray(mo)
        m[at] ^= 0x04
        yield bytes(m)
    yield mo + b"\0"                                    # a byte after the signature
    yield mo[:-1]
    for field, delta in ((8, 16), (8, -16), (12, 16), (12, -16)):   # the signature's range, moved or resized
        m = bytearray(mo)
        struct.pack_into("<I", m, cmd + field, struct.unpack_from("<I", m, cmd + field)[0] + delta)
        yield bytes(m)
    yield from unsealed_cases(mo)
    ncmds, sizeofcmds = struct.unpack_from("<II", mo, 16)
    m = bytearray(mo)                                   # a second LC_CODE_SIGNATURE in the header padding
    m[32 + sizeofcmds:32 + sizeofcmds + 16] = mo[cmd:cmd + 16]
    struct.pack_into("<II", m, 16, ncmds + 1, sizeofcmds + 16)
    yield bytes(m)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    tool = sys.argv[1]
    corpus = os.path.join(REPO, "tests", "fuzz", "corpus")
    with tempfile.TemporaryDirectory(prefix="hao-codesig.", dir=sys.argv[2] if len(sys.argv) == 3 else None) as work:
        cases = []                                      # (kind, name, bytes)
        for name in sorted(os.listdir(os.path.join(corpus, "codesig"))):
            blob = open(os.path.join(corpus, "codesig", name), "rb").read()
            cases += [("b", name, c) for c in blob_cases(blob)]
        for name in sorted(os.listdir(os.path.join(corpus, "macho"))):
            data = open(os.path.join(corpus, "macho", name), "rb").read()
            mo = arm_slice(data)
            if mo is None:
                continue
            cases.append(("m", name, data))             # as it is in the corpus (a fat file among them)
            cases += [("m", name, c) for c in macho_cases(bytes(mo))][1:]
        with open(os.path.join(work, "list"), "w") as lst:
            for k, (kind, _, data) in enumerate(cases):
                path = os.path.join(work, "%d.bin" % k)
                with open(path, "wb") as f:
                    f.write(data)
                lst.write("%s %s\n" % (kind, path))
        env = dict(os.environ, ASAN_OPTIONS="symbolize=0:detect_leaks=0")
        r = subprocess.run([tool, "--codesig", os.path.join(work, "list")], capture_output=True, text=True, env=env, timeout=600)
        if r.returncode != 0:
            sys.exit("FAIL %s --codesig: status %d: %s" % (tool, r.returncode, r.stderr.strip()[-600:]))
        got = r.stdout.splitlines()
    if len(got) != len(cases):
        sys.exit("FAIL %d cases, %d answers from %s" % (len(cases), len(got), tool))
    bad, verdicts = 0, {}
    for name in sorted(os.listdir(os.path.join(corpus, "macho"))):
        mo = arm_slice(open(os.path.join(corpus, "macho", name), "rb").read())
        sig = mo is not None and signature_command(bytes(mo))
        for m in unsealed_cases(bytes(mo)) if sig else ():
            if not hao_codesig.unsealed_use(m, sig[1]):
                bad += 1
                print("FAIL %s: tools/hao_codesig.py does not see a range reaching into the signature" % name)
    for (kind, name, data), c_line in zip(cases, got):
        py_line = blob_line(data) if kind == "b" else macho_line(data)
        verdicts[c_line.split()[0]] = verdicts.get(c_line.split()[0], 0) + 1
        if py_line != c_line:
            bad += 1
            if bad <= 10:
                print("FAIL %s (%d bytes): src/libval.c says '%s', tools/hao_codesig.py says '%s'" % (name, len(data), c_line, py_line))
    counts = ", ".join("%d %s" % (n, v) for v, n in sorted(verdicts.items()))
    if bad or "valid" not in verdicts or "invalid" not in verdicts or "refused" not in verdicts:
        sys.exit("FAIL %d of %d cases differ (%s)" % (bad, len(cases), counts))
    print("%d cases, the same answer from src/libval.c and tools/hao_codesig.py (%s)" % (len(cases), counts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
