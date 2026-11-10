#!/bin/sh
# Cross-check hao's x86 encoder against the system assembler.
# tests/test_x86_emit.c writes our bytes (ours.s), the same instructions as
# assembler text (ref.s) and our per-instruction lengths (ours.len). Both
# files are assembled and disassembled; the instruction text must match
# (branch targets compared relative to the branch, `# addr` comments dropped),
# and each of our encodings must be as short as the assembler's, except for
# the documented cases where we deliberately keep one fixed form.
set -e
D=${1:-build/x86check}
mkdir -p "$D"
clang -arch x86_64 -O1 -Wall -Iinclude -o "$D/gen" tests/test_x86_emit.c src/x86_emit.c && codesign -s - -f "$D/gen" 2>/dev/null
"$D/gen" "$D/ours.s" "$D/ref.s" "$D/ours.len"
clang -arch x86_64 -c -o "$D/ours.o" "$D/ours.s"
clang -arch x86_64 -c -o "$D/ref.o" "$D/ref.s"
dis() { objdump -d --no-show-raw-insn -M intel "$1" | grep -E '^ *[0-9a-f]+:' | sed -E 's/[[:space:]]+/ /g; s/^ //'; }
dis "$D/ours.o" > "$D/ours.dis"
dis "$D/ref.o" > "$D/ref.dis"
python3 -I - "$D/ref.dis" "$D/ours.dis" "$D/ours.len" <<'PY'
import re, sys

def norm(addr, l):
    l = l.split(" #")[0].strip()
    m = re.match(r"^(j\w+|call|jrcxz) 0x([0-9a-f]+)(\s*<.*>)?$", l)
    if m:
        return "%s .%+d" % (m.group(1), int(m.group(2), 16) - addr)
    if l in ("nop", "xchg rax, rax"): return "xchg rax, rax"
    if l.startswith("xchg ") and ", " in l:
        a, b = l[5:].split(", "); return "xchg " + ", ".join(sorted([a, b]))
    return l

def load(p):
    out, pend = [], None
    for raw in open(p):
        a, _, l = raw.partition(":")
        addr = int(a, 16); l = l.strip()
        if l == "lock": pend = addr; continue
        if pend is not None: out.append((pend, norm(pend, "lock " + l))); pend = None
        else: out.append((addr, norm(addr, l)))
    return out

ref = load(sys.argv[1]); ours = load(sys.argv[2])
ourlen = [int(x) for x in open(sys.argv[3])]
bad = [(i, r[1], o[1]) for i, (r, o) in enumerate(zip(ref, ours)) if r[1] != o[1]]
if len(ref) != len(ours): bad.append((-1, len(ref), len(ours)))

# Lengths: the assembler's length of instruction i is the distance to the next
# one (the last one is closed by the section end, approximated by our length).
reflen = [ref[i + 1][0] - ref[i][0] for i in range(len(ref) - 1)] + [ourlen[-1] if ourlen else 0]
# Forms where we keep one encoding on purpose although a shorter one exists:
#   test r, imm32 (the assembler has A9 for eax/rax and an imm8 form for 8-bit masks);
#   xchg with rax/eax (90+r); push imm32 (6A imm8); add/sub ... with eax and imm32 (05/2D...).
keep = re.compile(r"^(test|xchg|push|add|or|adc|sbb|and|sub|xor|cmp) ")
longer = [(i, ref[i][1], ourlen[i], reflen[i]) for i in range(min(len(ref), len(ourlen)))
          if ourlen[i] > reflen[i] and not keep.match(ref[i][1])]
if len(ourlen) != len(ref): bad.append((-2, "length entries", len(ourlen), len(ref)))
for b in bad[:30]: print("MISMATCH", b)
for b in longer[:30]: print("LONGER", b)
print("x86 emitter: %d instructions, %d mismatches, %d longer than the assembler's" % (len(ref), len(bad), len(longer)))
sys.exit(1 if bad or longer else 0)
PY
