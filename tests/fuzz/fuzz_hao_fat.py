#!/usr/bin/env python3
"""fuzz_hao_fat.py [--time S] [--seed N] [--artifacts DIR] CORPUS_DIR | FILE...

Random-mutation fuzzer for tools/hao_fat.py, which hao-embed and the image
installer (as root) run on every program in a System volume, and for
hao-embed's own Mach-O readers (eligible, carried_sections, uses_objc).
Inputs are mutated fat/thin Mach-O files (seeds: tests/fuzz/corpus/fat and
corpus/macho). For each input, slices(), has_cpu(), combine() with a small
x86_64 stub and replace_slice() must either succeed or raise ValueError;
any other exception is a crash. Results must also be consistent: every slice lies
inside the input, and a combined file parses back to the same slices plus
the stub. Crashing inputs are written to DIR (default ./) as
crash-<sha>. With only FILE arguments each file is run once.

The same inputs go through the other pure-Python packaging readers, which
must return or refuse with their one error type and never hang (the 2 s
alarm) or raise anything else:
  tools/hao_macho.py    Image (load commands, exports trie, each import
                        table: chained fixups, bind opcodes, symbol table),
                        file_slices and Cache, on every slice and the file
  tools/hao_codesig.py  info never raises; code_directories, special_slots
                        and bind_special_slots raise only ValueError
  tools/hao_ifunit.py   macho_uuid_of and read raise only Bad; read runs
                        again with the size and payload hash made valid, so
                        mutations reach the record rules (seeds: corpus/ifunit)
  tools/hao-install.py  macho_uuid returns a UUID or None
  kext/stamp_identity.py  commands (the shared walk at an offset)
"""
import argparse
import hashlib
import os
import random
import signal
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import hao_codesig  # noqa: E402
import hao_fat  # noqa: E402
import hao_ifunit  # noqa: E402
import hao_macho  # noqa: E402
from importlib.machinery import SourceFileLoader  # noqa: E402

TOOLS = os.path.dirname(hao_fat.__file__)
embed = SourceFileLoader("hao_embed", os.path.join(TOOLS, "hao-embed")).load_module()
install = SourceFileLoader("hao_install", os.path.join(TOOLS, "hao-install.py")).load_module()
stamp = SourceFileLoader("stamp_identity", os.path.join(TOOLS, "..", "kext", "stamp_identity.py")).load_module()
SCRATCH = None   # one file, rewritten per input, for the readers that take a path

STUB = (struct.pack("<IIIIIIII", hao_fat.MH_MAGIC_64, hao_fat.CPU_X86_64, 3, 2, 0, 0, 0, 0) + b"\0" * 32)


def check(data):
    sl = hao_fat.slices(data)
    if sl is not None:
        for cpu, sub, payload in sl:
            assert 0 <= len(payload) <= len(data), "slice larger than the file"
            assert isinstance(cpu, int) and isinstance(sub, int)
    hao_fat.has_cpu(data, hao_fat.CPU_ARM64)
    for drop in (False, True):
        try:
            out = hao_fat.combine(data, STUB, drop_x86_64=drop)
        except ValueError:
            continue
        back = hao_fat.slices(out)
        assert back is not None, "combine output does not parse"
        kept = [s for s in sl if not (drop and s[0] == hao_fat.CPU_X86_64)]
        assert [(c, s, bytes(p)) for c, s, p in back] == [(c, s, bytes(p)) for c, s, p in kept] + [
            (hao_fat.CPU_X86_64, 3, STUB)], "combine changed a slice"
    try:
        hao_fat.replace_slice(data, hao_fat.CPU_ARM64, b"\xcf\xfa\xed\xfe" + b"\0" * 28)
    except ValueError:
        pass
    for translate in (False, True):
        embed.eligible(data, translate)
    for f in (embed.carried_sections, embed.uses_objc):   # hao-embed reports ValueError as "fail"
        try:
            f(data)
        except ValueError:
            pass
    check_readers(data, sl)


def refuses(error, f, *args):
    """f(*args), or None where it refuses with its documented error type; anything else propagates as a finding"""
    try:
        return f(*args)
    except error:
        return None


def check_image(mo, what):
    """hao_macho.Image over one thin Mach-O: every table, each import route"""
    im = refuses(ValueError, hao_macho.Image, lambda o, n: mo[o:o + n], 0, what)
    if im is None:
        return
    for name, kind in (refuses(ValueError, im.exports) or {}).items():
        assert isinstance(name, str) and kind[0] in ("def", "reexport", "resolver")
    # imports() takes the first table present: chained fixups, bind opcodes, then the symbol table
    for drop in ((), ("fixups",), ("fixups", "dyldinfo")):
        for attr in drop:
            setattr(im, attr, None)
        for o, name, weak in refuses(ValueError, im.imports) or []:
            assert isinstance(o, int) and isinstance(name, str) and isinstance(weak, bool)
    im.section_of(im.base)


def check_codesig(mo):
    i = hao_codesig.info(mo)   # documented: None for a non-Mach-O, malformed reads as unsigned; never raises
    assert i is None or set(i) == {"signed", "flags", "platform", "team", "entitlements"}
    hao_codesig.library_validated(i), hao_codesig.dyld_env_blocked(i)
    for cd, hash_type, hash_size, nspecial, hash_off in refuses(ValueError, hao_codesig.code_directories, mo) or []:
        assert 0 <= cd and cd + hash_off <= len(mo) and hash_size * nspecial <= hash_off, "CodeDirectory outside the file"
    for slot in (hao_codesig.CSSLOT_INFOSLOT, hao_codesig.CSSLOT_RESOURCEDIR):
        for h in (refuses(ValueError, hao_codesig.special_slots, mo, slot) or {}).values():
            assert len(h) in hao_codesig.HASH_SIZES.values()
    out = refuses(ValueError, hao_codesig.bind_special_slots, mo, {1: b"plist", 3: None})
    assert out is None or len(out) == len(mo), "binding a slot changed the size"


def check_ifunit(data):
    refuses(hao_ifunit.Bad, hao_ifunit.macho_uuid_of, data)
    refuses(hao_ifunit.Bad, hao_ifunit.read, data)
    if len(data) >= hao_ifunit.HDR.size:   # as tests/fuzz/fuzz_ifunit.c: reach the rules behind the size and hash
        d = bytearray(data)
        struct.pack_into("<QQ", d, 48, len(d), hao_ifunit.hash64(bytes(d[hao_ifunit.HDR.size:])))
        got = refuses(hao_ifunit.Bad, hao_ifunit.read, bytes(d))
        if got:
            assert len(got[0]) == 32 and all(isinstance(r[0], str) for r in got[1])


def check_readers(data, sl):
    for k, (_, _, payload) in enumerate((sl or [])[:8]):
        mo = bytes(payload)
        check_image(mo, "slice %d" % k)
        check_codesig(mo)
    if not sl:
        check_image(data, "input")
        check_codesig(data)
    check_ifunit(data)
    for off in (0, 4096):
        for cmd, o, size in refuses(ValueError, lambda: list(stamp.commands(data, off))) or []:
            assert size >= 8 and off + 32 <= o and o + size <= len(data), "load command outside the file"
    if SCRATCH:
        with open(SCRATCH, "wb") as f:
            f.write(data)
        for cpu, sub, off in hao_macho.file_slices(SCRATCH):
            assert 0 <= cpu < 1 << 32 and 0 <= sub < 256 and off >= 0
        u = install.macho_uuid(SCRATCH)
        assert u is None or len(u) == 32
        if data[:7] == b"dyld_v1":
            cache = refuses(ValueError, hao_macho.Cache, SCRATCH)
            if cache:
                for p, _ in cache.paths[:16]:
                    refuses(ValueError, cache.image, p)
                for m in cache.files:
                    m.close()
    hao_macho.SECT_INSTR.clear()


INTERESTING = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x1000, 0x4000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
               0xFFFFFFFFFFFFFFFF, 0x0100000C, 0x01000007, 0xCAFEBABE, 0xCAFEBABF, 0xFEEDFACF]


def mutate(rng, d, corpus):
    d = bytearray(d)
    for _ in range(rng.randint(1, 6)):
        r = rng.randrange(8)
        # most structure lives in the first page (fat header and arch table)
        hot = min(len(d), 0x1000) if rng.random() < 0.8 else len(d)
        if r == 0 and d:
            d[rng.randrange(hot)] ^= 1 << rng.randrange(8)
        elif r == 1 and len(d) >= 8:
            w = rng.choice((4, 8))
            o = rng.randrange(0, max(1, hot - w)) & ~3
            v = rng.choice(INTERESTING) & ((1 << (8 * w)) - 1)
            d[o:o + w] = v.to_bytes(w, rng.choice(("big", "little")))
        elif r == 2 and d:
            d = d[:rng.randrange(len(d))]
        elif r == 3:
            o = rng.randrange(len(d) + 1)
            d[o:o] = bytes(rng.randrange(256) for _ in range(rng.randint(1, 32)))
        elif r == 4 and len(d) > 8:
            a, n = rng.randrange(len(d)), rng.randint(1, 64)
            del d[a:a + n]
        elif r == 5 and len(d) >= 28:
            # one fat_arch field (fat32 entries are 20 bytes from offset 8)
            k = rng.randrange(4)
            o = 8 + 20 * k + 4 * rng.randrange(5)
            if o + 4 <= len(d):
                d[o:o + 4] = rng.choice(INTERESTING).to_bytes(8, "big")[-4:]
        elif r == 6 and corpus:
            other = rng.choice(corpus)
            a = rng.randrange(min(len(d), len(other)) + 1)
            d[a:a + 64] = other[a:a + 64]
        elif r == 7 and len(d) >= 8:
            d[4:8] = struct.pack(">I", rng.choice((0, 1, 2, 3, 4, 64, 0x10000, 0xFFFFFFFF)))
    return bytes(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--time", type=float, default=60)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--artifacts", default="./")
    ap.add_argument("inputs", nargs="+")
    a = ap.parse_args()
    global SCRATCH
    scratch_dir = tempfile.TemporaryDirectory(prefix="hao-fuzz-")
    SCRATCH = os.path.join(scratch_dir.name, "input")
    corpus, dirs = [], 0
    for p in a.inputs:
        if os.path.isdir(p):
            dirs += 1
            corpus += [open(os.path.join(p, n), "rb").read() for n in sorted(os.listdir(p)) if not n.startswith(".")]
        else:
            corpus.append(open(p, "rb").read())
    if not dirs:
        for d in corpus:
            check(d)
        print(f"fuzz_hao_fat: ran {len(corpus)} inputs")
        return 0
    seed = a.seed if a.seed is not None else int(time.time())
    rng = random.Random(seed)
    for d in corpus:
        check(d)
    t0, runs, crashes, kinds = time.time(), 0, 0, {}

    def hang(sig, frame):
        raise TimeoutError("input took more than 2 s (hang)")
    signal.signal(signal.SIGALRM, hang)
    while time.time() - t0 < a.time:
        d = mutate(rng, rng.choice(corpus), corpus)
        runs += 1
        try:
            signal.alarm(2)
            check(d)
            signal.alarm(0)
        except Exception as e:  # noqa: BLE001 - every unexpected exception is a finding
            signal.alarm(0)
            crashes += 1
            key = f"{type(e).__name__}: {e}"[:120]
            if key not in kinds:
                kinds[key] = d
                path = os.path.join(a.artifacts, "crash-" + hashlib.sha256(d).hexdigest()[:16])
                open(path, "wb").write(d)
                print(f"fuzz_hao_fat: {key} -> {path}", flush=True)
    el = time.time() - t0
    print(f"fuzz_hao_fat: seed {seed}: {runs} runs in {el:.0f}s ({runs / el:.0f}/s), "
          f"{crashes} crashing inputs, {len(kinds)} distinct")
    return 1 if crashes else 0


if __name__ == "__main__":
    sys.exit(main())
