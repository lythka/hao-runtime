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
"""
import argparse
import hashlib
import os
import random
import signal
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import hao_fat  # noqa: E402
from importlib.machinery import SourceFileLoader  # noqa: E402

embed = SourceFileLoader("hao_embed", os.path.join(os.path.dirname(hao_fat.__file__), "hao-embed")).load_module()

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
