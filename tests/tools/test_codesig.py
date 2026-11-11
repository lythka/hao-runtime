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
    byte appended, the last byte removed, the signature's range moved, and a
    second LC_CODE_SIGNATURE.
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
