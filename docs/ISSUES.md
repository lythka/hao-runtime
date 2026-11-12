# Runtime issues and measured scope

## Per-entry jump-cache base in translated code

Each translated indirect branch previously loaded `cpu.rt_jc`, then its
selected cache slot, then the entry PC and code. The cache mapping is
per-thread and stable for the lifetime of an entry and its chained blocks.
The entry stub now loads its base into host R12, which was already saved
and restored by that stub; the hot branch reads the slot directly through
R12. This removes one dependent memory load per BR/BLR/RET. The slot's
full-PC comparison, miss probe, PC store and publication rules are unchanged.
Helpers preserve R12 by the host ABI. Nested callback/signal entries save
and restore it, and a new dispatcher entry binds and reloads its own cache.
No guest integer/vector register is moved out of `hao_cpu`, no new fault
recovery location is introduced, and AOT code uses the same entry contract.

`state_bench` covers two alternating return sites with compact and
8-KiB-spaced table targets, BR tail dispatch, a single return site, and
short arithmetic blocks around XPACI. The helper loop has ordinary guest
register loads/stores around the call, not a full CPU-state flush. Native
arm64 and JIT/interpreter/helpers-only/AOT results agree. Separate `--stats`
runs count eight startup helper calls in the call/return workloads and
N+8 in the XPACI workload, both before and after. The host dispatcher is
not entered per hot call: its block count stays at the startup-scale count
while the iteration count grows.

Two-second, 1-ms `sample` captures on Rosetta put 1,016/1,463 main-thread
samples before and 1,041/1,483 after in `jit_helper` and its callees for the
short helper loop. Call/return samples stay in generated Rosetta code;
those addresses do not identify individual hao x86 state moves. AOT
inspection confirms the repeated cache-base load and the existing
per-instruction register stores. Guest-register copying and C dispatch
have not been removed. Cross-block guest-register caching and reduced
helper state materialization are not implemented: the profiles did not
establish a benefit that justified new signal/fault recovery rules.

Performance is provisional. Repeated Rosetta measurements vary materially
between processes, including the helper loop whose hot path this change
does not alter. The results do not establish either a general speedup or
a regression. The implementation is retained for review on the strength
of the completed correctness checks; further performance validation is
needed. Alternative cache-base and helper-pointer register sequences were
also measured, without a reliable basis for selecting them.


One measurement set after the test workload stopped: median ns/iteration,
seven interleaved runs, 100 million iterations per pattern, without
`--stats`. These are observations, not acceptance thresholds or established
performance effects.

| Pattern | JIT before -> after | AOT before -> after |
| --- | --- | --- |
| BLR, two return sites | 5.799 -> 5.675 | 5.957 -> 5.834 |
| BLR, spaced targets | 5.784 -> 5.803 | 5.971 -> 5.562 |
| BR tail dispatch | 5.163 -> 5.675 | 5.683 -> 5.706 |
| XPACI helper blocks | 9.945 -> 9.603 | 10.114 -> 9.709 |
| BLR, one return site | 5.783 -> 4.941 | 4.814 -> 5.466 |

For example, the seven baseline single-return AOT observations range
from 4.389 to 7.586 ns; the candidate ranges from 5.266 to 5.715 ns.
Uncontrolled host/Rosetta effects prevent interpreting the median
difference alone as a regression.

Validation includes the full all-mode oracle and sanitizer oracle,
`make check`, native equality in all five benchmark patterns, and
`signals.sh`, `uresume.sh`, `threads.sh`, `fullcache.sh`, `chain.sh` and
`jitlock.sh`. The manifest reports 76 passed, one expected failure and
three expected skips, with no unexpected failures or fork type-at crash.
Intel hardware, application frame latency and release-image
acceptance remain outside this measured scope.

## Inline FP32 FMLA/FMLS in translated code

Vector FMLA/FMLS .2S/.4S, three-same and by element, now emit the
`fp_vec32_muladd` fast path in `src/jit.c`. Integer admission precedes FP
arithmetic for every active lane. Exact binary64 products, an RN sum,
TwoSum error recovery and an integer round-to-odd step precede one
`cvtpd2ps` per pair under guest rounding/FZ. FMLS flips the multiplicand
sign exactly. The indexed single form uses H:L for the lane and M:Rm for
V0..V31; half precision's restricted Vm encoding is not reused for S.
A .2S result clears the upper half. Nonordinary operands, unsupported
FPCR controls and tiny/zero results retain the existing helper descriptor
and restore the pre-attempt FP environment. Scalar, half and double fused
forms remain helpers. No VEX/AVX instructions or absolute addresses are
emitted; integer branches preserve live NZCV and deterministic re-emission.

The common RN path saves MXCSR but does not reload it; other rounding
modes use the second word of the entry stub's eight-byte slot for RN and
restore guest RC before conversion. The block, oracle scratch and PC
recovery buffers have a common 128 KiB allowance: a maximum 96-instruction
block of these expansions exceeded the previous 64 KiB oracle scratch.
`--stats` adds `helper_calls` for executed interpreter-helper calls, enabled
only when statistics are requested; `helper` remains a translation count.

Validation: the vector unit test checks 303,682 complete-state cases,
including all 16 FPCR settings, full FPSR, directed near-ties, aliases,
every indexed lane, high Vm and maximum-sized blocks. Independent mutations
of the odd step, product negation and H:L decode produce 11,078, 46,930 and
37,404 mismatches respectively. The encoder check covers 39,307 encodings.
`fma32.sh` (226 output lines) and extended `jitinsns.sh` (489 lines) compare
native arm64 with JIT, interpreter, helpers-only and AOT. The million-record
fma/simd/block corpora replay with zero mismatches and unchanged IEEE-flag
difference counts versus 2207f5e: interpreter 0/0/0, JIT 0/53/24,592.
The nonzero counts are existing differences, not new fused-operation
exceptions. Full runtime gates include the all-mode oracle, sanitizer
oracle, unit, AOT, guest, host and packaging checks.

Median ns/FMLA on Rosetta, seven interleaved runs per variant, sixteen
million instructions per form per run, without `--stats`:

| Form | JIT before -> after | AOT before -> after |
| --- | --- | --- |
| Three-same .4S | 23.385 -> 13.123 | 23.228 -> 13.153 |
| By-element .4S | 25.071 -> 13.128 | 24.999 -> 13.047 |

Both runs use the same normal inputs and produce the native arm64 checksum.
The final runtime gate passes with only the documented skips and expected
failure; no `test_fork` type-at crash was observed. A fresh parallel build
exposed a test dependency race (`jitlocktest` started before `hao` existed);
its Makefile prerequisite now includes the executable.

The normal-input benchmark executes sixteen fused operations per loop.
Separate `--stats` runs of four million operations per form report
8,000,000 -> 0 executed helper calls for both JIT and AOT (the baseline
was instrumented with the same counter only). JIT translated helper sites
fall from 64 to 0; AOT execution has no JIT-site counts. The benchmark's
translated code grows from 6 KiB to 48 KiB, including fallback paths.

Limits: the arithmetic is still a multi-instruction SSE2 expansion, with
substantial code size and a helper for cancellation, tiny values and
special operands. Measurements are on the Apple silicon development host
running hao under Rosetta, not on Ivy Bridge. Instruction-cache effects
in larger applications and application/release-image acceptance are not
established by these microbenchmarks or runtime gates.

## Swift values with an Array-supplied nil, spare-bit enum tags or an extra tag byte

The Swift value walk (`src/swiftvalue/Recipe.c` `selected`/`collect`, run
on a published value and on a guest call of a native value witness) refused
three shapes, and a refusal at publication ends the process:

- An `Optional` of a struct whose extra inhabitants come from an `Array`
  field. Swift takes a struct's inhabitants from its first field with the
  largest count, and an `Array` ties with most pointer-sized fields and
  comes first. `selected()` planned an `Array` buffer word only for an enum
  Element (any Element on the witness path).
- A compiler-laid-out multi-payload enum whose tag is in the payloads'
  spare bits, for example `SwiftUI.ContainerBackgroundValue.Content`
  (16 bytes, flags 0x210007; a class-reference word, or two of them).
- A compiler-laid-out multi-payload enum with an extra tag byte, for
  example `String` / `(String, String)` / one empty case (33 bytes, stride
  40, flags 0x2230007, xi 0xfd).

`enum_payloads()` accepts only runtime-laid-out enums (the runtime's
multi-payload tag witness and a payload-size word in the metadata), which
the two enum shapes do not have.

Measured in one program built natively for arm64 and for x86_64
(`tests/host/swiftlayout.swift`, arm64 natively and x86_64 under Rosetta
on the development host):

- `Array<E>` is one buffer word, size 8, flags 0x10007, xi 0x7fffffff, for
  E a struct, an enum, a Swift class, `AnyObject`, `NSObject` and `Any`.
  XI0 is 0 on both; XI k is k on arm64 and 2k on x86_64. `Optional<S>.none`
  of a struct whose first field is the `Array` writes only that word.
- Spare-bit tags: for the 16-byte enum above (and its SwiftUI original)
  injecting case 0 into zeroed storage writes nothing and case 1 sets bit 63
  of word 1 on both; live values are `[p, 0]` and `[p, 0x8000000000000000|p]`
  on both. A five-case enum of one class reference uses bits 61-63 on both.
  A 17-case one uses bits 60-63 and 2 on arm64, 59-63 on x86_64. The empty
  encodings always differ: xi 0x3ffe against 0x3ffffe for the 16-byte enum,
  XI0 words f000000000000007 against ff00000000000007.
- Extra tag byte: payload cases leave the payload as it is and write the
  case index into byte 32; the empty case is a zero payload with tag 2; the
  inhabitants are tag bytes ff, fe, ... Identical on both, live values too.
  With two empty cases xi is still 0xfd (they share tag 2 and keep their
  index in the payload area).

Rules as implemented:

- A: the `Array` branch of `selected()` admits any Element on both paths.
  It plans the buffer word's empty encodings only. A live `Array` is not
  walked: its elements are still not certified, and an outer nil inside an
  element is no patch of the enclosing value.
- B (`Enum.c` `spare_bit_case`): an enum whose field descriptor is
  MultiPayloadEnum, with 2 to 64 direct payload cases that all resolve and
  no empty case; size a multiple of 8 up to 128 bytes, equal to its stride,
  alignment 8; every value witness (0-7, 11-13) native, in no guest image
  or thunk. Each payload case injected with the native witness into zeroed
  storage must read back as that case and set no bit outside bits 60-63 of
  a word. The live case is the native `getEnumTag`; that case's payload is
  walked as its type from a copy with every injected tag bit cleared
  (`destructiveProjectEnumData` is never run on the live value). A patch
  inside it that would overwrite a tag bit refuses the value.
- C (`Enum.c` `tag_byte_case`): an enum whose field descriptor is
  MultiPayloadEnum, with 2 to 64 direct payload cases that all resolve and
  at most one empty case; size = largest payload + 1 and xi = 256 - (payload
  cases + empty cases), which in Swift's layout means no spare bit carries
  the tag. The byte at the largest payload's size is the case: a payload
  case's payload is walked as its type at offset 0, the empty case needs
  nothing, any other byte is refused. This is layout only, so it applies to
  a guest enum whose witnesses are arm64 code as well.
- D (`Recipe.c` `collect_inner`): a value of fewer than 8 bytes is walked
  with no patch and no verdict on its case. A patch rewrites a pointer word,
  and such a value holds none. Before this rule every enum of that size
  other than a payload-free one was refused, for example a 1-byte guest
  enum with payload cases (flags 0x2200000) inside a SwiftUI value, and an
  `Optional<Bool>` published as a result. Either refusal ended the process.
  `swiftlayout` measures a single-payload enum (1 byte, xi 0xfc), a Bool
  multi-payload enum (2 bytes), a 6-byte struct of them and an `Optional` of
  the first. Each is identical on both architectures. Those 8 walks and 2
  of `Optional<Bool>` were refused before this rule.

Both enum rules are used by the walk only. The ownership transfers in
`Aggregate.c` and `graph()`, which picks the types whose native witnesses a
publication intercepts, still follow only runtime-laid-out enums.

`tests/host/swiftlayout.sh` (new in `tests/check.list`) checks the
measurements on each architecture, that every other measured line is equal
on both, and 46 walk verdicts over the x86_64 values. Before the change 36
of them were refused: every `Optional` of an `Array`-first struct except
an enum Element, every enum value of shape B and C, nil patches inside
their payloads included. Still refused, as expected: the 17-case enum, an
`Optional` nil of the B enum and of its SwiftUI original, a spare-bit enum
with an empty case, the extra-tag enum with two empty cases, copies of its
metadata with size + 1 or xi - 1, and a tag byte past the cases.
`tests/guest/optwitness.sh` now also copies, assigns and drops tuples of the
C-shaped guest enum and a `C??` through the native tuple witnesses (arm64e
guest under JIT, `--interp` and AOT against native arm64). The walk must get
through the enum to give the `C??` outer nil its native encoding. Before the
change all three modes crashed in `swift_retain(0x1)`; now all 111 lines are
equal.

Remaining limitations:
- Measured on the development host's native Swift runtimes (arm64, and
  x86_64 under Rosetta), not on the target system or Intel hardware.
- An `Optional` of a B or C enum is refused. For B its nil encodings
  differ. For C they are tag bytes that are the same on both, but not part
  of this rule.
- B: a guest-defined spare-bit enum (arm64 witnesses), an empty case, a tag
  that needs a bit outside 60-63 and an enum over 128 bytes are refused.
  The tag-bit patch refusal is not exercised: the payload words that held a
  nil in the measured enums had no spare bits, so no tag landed in them.
- C: only one empty case was measured, and more are refused.

## Table lookups, EXT, integer multiplies and round-to-integral in translated code

Translated code ran TBL/TBX, EXT, the integer vector multiplies (MUL, MLA,
MLS, vector and by element) apart from `MUL .8H/.4H`, every FRINT* (vector
and scalar), the 64-bit lane compares (CMGT, CMGE, CMHI, CMHS, CMTST and the
compares with zero on `.2D`), CMHI/CMHS on 32-bit lanes, SMIN/SMAX/UMIN/UMAX
other than signed halfwords and unsigned bytes, and ABS through the
interpreter helper. None of them has an SSE2 form; all have one in SSSE3,
SSE4.1 or SSE4.2, which hao now requires of the CPU (x86-64-v2, checked at
startup by `src/main.c`; no VEX encodings are used).

`src/jit.c` now emits them inline (encoders `x_sse38_rr`, `x_sse3a_rri` in
`src/x86_emit.c`):
- TBL/TBX with one to four table registers, `.8B`/`.16B`: one PSHUFB per
  table register on the indices minus 16k, a saturating add of 0x70 setting
  bit 7 (zero result) for indices outside that register; the results are
  ORed. TBX keeps Vd's byte where the index is at least 16 x len.
- EXT: PALIGNR on Vm:Vn; the 64-bit form rotates the two low halves joined.
  EXT with Q = 0 and imm4 >= 8 is reserved: the interpreter returned
  uninitialized bytes of its staging buffer there and now reports UNDEF as
  hardware does (the oracle's leniency entry for it is removed).
- MUL/MLA/MLS on bytes (two PMULLW on the even and odd bytes), halfwords
  (PMULLW) and words (PMULLD); MUL/MLA/MLS by element on halfwords and words.
- CMEQ/CMTST/CMGT/CMGE (PCMPEQQ/PCMPGTQ) and CMHI/CMHS (the same after
  flipping the sign bits) on `.2D`, CMHI/CMHS on `.4S`/`.2S`, the compares
  with zero on `.2D`; SMIN/SMAX/UMIN/UMAX in every lane size (PMINSB, PMINSD,
  PMINUW, PMINUD and the maxima); ABS (PABSB/W/D; `.2D` as (x ^ s) - s).
- FRINTN/M/P/Z/X/I and FRINTA, vector `.2S`/`.4S`/`.2D` and scalar S/D: the
  fast path of `fp_rint`. Operands with any lane that is subnormal, infinite
  or NaN go to the helper before any FP instruction runs, with nothing
  raised (FZ/IDC, FPProcessNaN and DN stay the interpreter's). For zero and
  normal lanes ROUNDPS/ROUNDPD is FPRoundInt exactly: the fixed modes for
  N/M/P/Z, MXCSR.RC (the guest's FPCR.RMode) for X and I, PE (IXC) only for
  FRINTX. FRINTA (ties away) is trunc(x) plus copysign(1, x) when
  |x - trunc(x)| >= 1/2, else plus x's signed zero; each step is exact for a
  normal x, so it raises nothing. No MXCSR save/restore is needed: a
  declined operand reaches no FP instruction.

Measured on the Apple silicon development host, where hao's x86_64 code
runs under Rosetta (not on Intel hardware), so these are ratios, not Intel
timings. `tests/guest/vbench2.c` (2000 repetitions, best of five, host
under load from other jobs):

| Kernel | Before, JIT | After, JIT | Before, AOT | After, AOT |
|---|---:|---:|---:|---:|
| TBL/TBX/EXT (`shuffle`) | 0.095 s | 0.006 s | 0.099 s | 0.005 s |
| MUL/MLA/MLS (`imul`) | 0.394 s | 0.020 s | 0.411 s | 0.020 s |
| FRINT* and `.2D` FP (`round`) | 2.010 s | 0.782 s | 2.074 s | 0.778 s |
| compares, min/max, ABS (`cmpmm`) | 0.700 s | 0.020 s | 0.730 s | 0.018 s |
| total | 3.22 s | 0.83 s | 3.31 s | 0.82 s |

Its static helper instructions went from 178 to 46 of 985 and its dynamic
helper calls (400 repetitions) from 20.1 M to 2.87 M, all of the remainder
double-precision vector FADD/FSUB. `tests/guest/fpscalar -b NAME 300000`
(ns per instruction under the JIT, best of three): `frintn v.4s` 31.7 to
3.6, `frintx s` 17.4 to 4.1, `frinti s` 16.1 to 3.7, `frintm d` 16.7 to
3.8, `frinta s` 18.7 to 8.1, `frinta v.2d` 23.9 to 7.3. `tests/guest/vbench.c`
is unchanged (0.39-0.40 s, JIT and AOT).

Validation: `tests/aot/jit_vector.c` (in `make unit`) now runs 200,000 cases
(120,000 before), adding the integer three-same compares/min/max/multiplies,
the compares with zero and ABS, TBL/TBX (indices mostly in or near range)
and EXT including the reserved encodings, the by-element multiplies with
their neighbours, and FRINT* vector and scalar (all keys, reserved ones
included) over values around the integers in single and double, alone,
before and after single-precision vector FP (the constants one leaves for
the next), under all 16 RMode/DN/FZ settings: translator and interpreter
agree on registers, NZCV, the full FPSR and memory. `tests/guest/jitinsns.sh`
compares 89 more lines (477) with native arm64 under JIT, `--interp`, helpers
only and AOT, FRINT* over halves, quarters and specials under six FPCR
settings among them. `tests/guest/fpscalar.sh` (every FRINT* over corner
values under each rounding mode with FZ and DN, against native arm64) passes
under JIT, `--interp`, helpers only and AOT. The oracle's simd and block
modes, one million records each, checked by the runtime before and after on
the same native records: no mismatches, and the same FPSR IEEE-flag
differences record for record (36 simd, 24,488 block; the known scalar
FZ/IDC gap); 365 more reserved EXT encodings now report UNDEF.

Remaining limitations:
- Double-precision vector arithmetic (FADD/FSUB/FMUL `.2D`, FMLA/FMLS) is
  still helper-bound; it is most of what is left in `vbench2`'s rounding
  kernel.
- FRINT32*/FRINT64*, half-precision FRINT*, PMUL, the pairwise integer
  min/max and the scalar (`D`-register) integer compares, ABS and NEG stay
  with the helper.
- A FRINT* operand with a subnormal, infinite or NaN lane pays the inline
  check and then the helper.

## Vector floating point and structure loads/stores in translated code

Translated code ran every Advanced SIMD floating-point instruction and every
structure load or store (LD1, ST1, LD1R, ...) through the interpreter
helper: an indirect call, `a64_exec`'s and `a64_exec_simd`'s decoders,
copies of the registers, and only then `fp_vec32`/`fp_vec32_select` or the
memory copies of `a64_ldst_simd`. Byte-lane and arithmetic 64-bit shifts,
the shift-and-accumulate and rounding shifts, SHRN, SSHLL/USHLL and XTN went
the same way. A single-precision kernel spent nearly all its time there.

`src/jit.c` now emits these inline, SSE2 only:
- FADD, FSUB, FMUL (vector and by element), FMAX, FMAXNM, FMIN, FMINNM,
  FCMEQ, FCMGE, FCMGT on `.2S`/`.4S`: the fast path of `fp_vec32` and
  `fp_vec32_select` instruction for instruction (integer admission of the
  operands, the SSE operation under the guest's MXCSR, the same result
  check, the MXCSR saved and restored around a declined result). A declined
  case calls the helper, which declines it again and takes the per-lane
  path, so values and FPSR are the interpreter's. The code does not touch
  EFLAGS; the declining path stores NZCV before the call and reloads it
  after only when it is live. FABS and FNEG (`.2S`, `.4S`, `.2D`).
- SHL, USHR, SSHR, USRA, SSRA, URSHR, SRSHR, URSRA, SRSRA by an immediate in
  every lane size (bytes and the arithmetic 64-bit forms emulated), SHRN,
  SHRN2, SSHLL/USHLL (UXTL/SXTL) and their "2" forms, XTN, XTN2.
- LD1/ST1 of one to four registers, LD1R, LD1/ST1 of one lane, without
  offset or post-indexed by an immediate or Xm, SP as base. Registers are
  transferred one at a time and the base written back last, as in
  `a64_ldst_simd`: a fault part way leaves the earlier registers transferred
  and Xn unchanged, at the instruction's pc.

Measured on the Apple silicon development host, where hao's x86_64 code
runs under Rosetta (not on Intel hardware): `tests/guest/vbench.c` (FADD,
FSUB, FMUL, FMUL by element, FMAX(NM), FMIN(NM), FCMGT, FCMGE, LD1/ST1 of
one to four registers, LD1R; 4000 repetitions) went from 1.40 s to 0.40 s
under the JIT and from 1.42-1.51 s to 0.39-0.40 s with an AOT cache (best of
five runs each, host under load from other jobs); its dynamic helper calls
from translated code went from 87,062,441 to 6,153 and its static helper
instructions from 124 to 21 of 436 (`--stats`). `tests/guest/bench.c`
(double-precision matrix code, FMLA) is unchanged: 53 to 45 static helper
instructions, 2.20 M to 2.15 M helper calls, wall time within noise.
`tests/aot/jit_vector.c` (in `make unit`) compares 120,000 cases of these
encodings, alone and between a CMP and a CSEL that reads NZCV, through the
translator and the interpreter under all 16 RMode/DN/FZ settings, with the
full FPSR (the IEEE flags the oracle only counts), all registers and memory,
including faulting accesses at a page end: no differences.
`tests/guest/jitinsns.sh` compares 107 more lines (281 before) with native
arm64 under JIT, `--interp`, helpers only and AOT: the vector FP forms over
a corner-value table under five FPCR settings with FPSR and NZCV, the
shifts, the loads and stores (post-index by immediate and register, SP
base) and a faulting post-indexed LD1 in a signal handler that skips or
repairs it.

Remaining limitations:
- Double-precision vector arithmetic, FMLA/FMLS (fused), FDIV, FMULX,
  FRECPS/FRSQRTS, FABD, FACGE/FACGT, the pairwise forms, half precision,
  conversions and the vector compares with zero still go through the
  helper, as do LD2-LD4/ST2-ST4 and LD2R-LD4R.
- A vector FP instruction with any special lane (NaN, infinity, subnormal,
  or a result that is zero, subnormal or near the minimum normal without
  being an exact zero) pays the inline check and then the helper.
- Translated loads and stores, like `t_ldst_reg`'s, use the address as
  given: a tagged (top-byte) address faults where the interpreter strips
  the tag.
- A 16- or 8-byte register store that straddles into an inaccessible page
  stores nothing in translated code; the interpreter's `memcpy` may store
  the bytes before the page end (Arm leaves them UNKNOWN). Registers before
  it in the list are stored by both.
- Each translated single-precision arithmetic instruction is about 250-350
  bytes of x86 code: `vbench`'s translations grew from 13 to 33 KB.

## Scalar FP conversions, max/min and round-to-integral cost

The translator emitted SSE code for scalar FADD/FSUB/FMUL/FDIV/FSQRT, FCVT
between single and double, FCMP, FCSEL and SCVTF. Every other scalar or
packed floating-point instruction ran through the interpreter helper, and
there the software model did all of the work: FP-to-integer conversions and
round-to-integral unpacked the operand to a 128-bit significand and rounded
it; integer-to-FP conversions went through the general `round_pack_mode`
even when the integer was exactly representable; max/min unpacked both
operands twice and repacked the winner; a compare with zero unpacked both
sides; and `nans()`, which every add, subtract, multiply, divide, max/min and
fused operation calls before anything else, classified each operand up to
four times (two ordered searches, signalling then quiet, each through the
format table) although almost no operation has a NaN operand.

What changed, in two layers.

Translated code (`src/jit.c`, single and double only; JIT and AOT emit the
same bytes). Each form is taken inline only for results the SSE instruction
can vouch for; anything else re-executes the instruction in the helper with
the operands untouched, and a declined attempt leaves in MXCSR only flags the
helper raises again:
- `FCVTZS`/`FCVTZU Wd|Xd, Sn|Dn`: `CVTTSS2SI`/`CVTTSD2SI`. Signed: a result
  equal to the minimum integer (the x86 integer indefinite) goes to the
  helper, which saturates and raises IOC where x86 raised IE. Unsigned: the
  operand's bits are admitted first (`0 <= x < 2^32` or `2^63`, one unsigned
  compare that also rejects negatives, infinities and NaNs), because a
  negative fraction or a double above 32 bits would raise PE where A64 raises
  only IOC. A zero result from a denormal operand goes to the helper (IDC
  instead of IXC under FZ).
- `UCVTF Sd|Dd, Wn|Xn`: `CVTSI2SS`/`CVTSI2SD` of the zero-extended value; an
  `Xn` with bit 63 set goes to the helper.
- `FMOV Sd|Dd, #imm`: a constant store.
- `FMAX`/`FMIN`/`FMAXNM`/`FMINNM`: `MAXSS`/`MINSS` for unequal ordered
  operands, AND (max) or OR (min) of the operands when they compare equal
  (the zero-sign rule). A denormal operand (tested on the bits) or a NaN
  (`UCOMIS` unordered) goes to the helper.
- Packed single (`.4S`, `.2S`): `SCVTF` is `CVTDQ2PS`; `UCVTF` the same when
  no lane has bit 31 set; `FCVTZS` is `CVTTPS2DQ` unless a lane is denormal
  or answers the integer indefinite; `FCMGT #0.0` is the signed integer
  compare of the bits with zero unless a lane is a NaN or denormal.

Interpreter and helper (`src/a64_fp.c`, single and double; half precision
keeps the reference path):
- `nans()` first asks, with one integer compare per operand, whether any
  operand is a NaN.
- `fp_to_int` (every FCVT*S/U rounding mode and the fixed-point forms) and
  `fp_rint` (every FRINT*) work on the bits of a normal operand with 64-bit
  shifts; zero, denormal, infinite, NaN and out-of-range operands keep the
  reference path, which owns IOC, IDC and saturation.
- `fp_from_int` uses the host conversion for an integer that fits `int64`
  with no fraction bits: one rounding in the MXCSR mode, PE for IXC.
- `fp_max`/`fp_min` and the NM forms return the selected operand for zeros,
  normals and infinities; `cmp3` compares sign-magnitude keys unless FZ could
  flush an operand.
- `fp_raise` writes MXCSR only when a flag is new.
- `FJCVTZS` judged its range before truncating: `-2^31 - 0.5` reported IOC,
  where A64 reports IXC (the truncated value is in range). It now tests the
  truncated integer.

Measured with `tests/guest/fpscalar -b NAME 300000` (a loop of eight copies
of the instruction, ns per instruction, best of three). The development host
is Apple silicon and runs hao under Rosetta, so these are ratios, not
Intel timings; `STMXCSR`/`LDMXCSR` in particular are much slower there than on
an Intel CPU.

| Instruction | Before, JIT | After, JIT | Before, AOT | After, AOT |
|---|---:|---:|---:|---:|
| `fcvtzs w, s` | 29.7 | 0.73 | 30.9 | 0.63 |
| `fcvtzu x, d` | 28.9 | 0.79 | 30.4 | 0.63 |
| `ucvtf s, w` | 25.3 | 0.32 | 26.3 | 0.23 |
| `fmov s, #1.0` | 13.7 | 0.38 | 13.8 | 0.29 |
| `fmaxnm s, s, s` | 32.6 | 3.49 | 34.8 | 3.45 |
| `scvtf v.4s` | 89.9 | 3.15 | 92.9 | 3.12 |
| `ucvtf v.4s` | 92.4 | 2.54 | 94.2 | 2.67 |
| `fcvtzs v.4s` | 31.2 | 3.80 | 32.3 | 3.42 |
| `fcmgt v.4s, #0.0` | 43.4 | 3.02 | 46.3 | 2.77 |
| `frintn v.4s` (helper) | 68.5 | 27.6 | 72.3 | 28.4 |
| `frintx s` (helper) | 26.2 | 15.4 | 28.8 | 15.6 |
| `ucvtf s, s` (helper) | 43.0 | 19.0 | 44.6 | 20.1 |
| `fcvtns w, s` (helper) | 31.1 | 23.0 | 31.8 | 23.3 |
| `fmaxnm v.2d` (helper) | 62.0 | 26.4 | 61.8 | 25.3 |
| `fmadd s` (helper) | 44.3 | 36.9 | 43.3 | 37.9 |
| `fdiv v.4s` (helper) | 88.3 | 85.1 | 84.2 | 81.1 |

`tests/guest/fpscalar.sh` runs 147 instruction forms (the ones above, every
FCVT* rounding mode, fixed point, every FRINT*, compares, max/min, pairwise
and NaN-selection cases, half-precision controls) over corner values, all
pairs of them for two-operand forms, and random values, under each rounding
mode with FZ and DN off and on and FPSR clear or full beforehand. Results,
FPSR (IOC, DZC, OFC, UFC, IXC, IDC, QC) and NZCV equal native arm64 under JIT,
`--interp`, helper-only JIT and AOT.

Remaining limitations:
- The non-truncating `FCVT*` modes, fixed-point conversions, `FCVTZU` and
  `UCVTF` with large lanes, the double packed conversions, the other
  compares with zero and packed max/min still go through the helper; its
  dispatch is then most of their cost. (`FRINT*` is now inline: see "Table
  lookups, EXT, integer multiplies and round-to-integral" above.)
- Inline SSE arithmetic that predates this change (FADD, FSUB, FMUL, FDIV,
  FSQRT, FCVT single<->double, FCMP) differs from A64 in FPSR under FZ: a
  flushed denormal operand does not raise IDC, and a result flushed to zero
  raises IXC together with UFC where A64 raises UFC alone. Values agree. The
  interpreter (`--interp`) is exact. `tests/guest/fpscalar.c` runs those
  instructions with FZ clear.
- FPCR.AH, FIZ and NEP are not modelled; the test leaves them clear.
- The packed tests for denormal lanes also send a lane equal to the minimum
  normal number to the helper (exact, slower).

## Single-precision fused multiply-add cost

The release CPU profile has no FMA instruction, so every fused
multiply-add ran in the exact software path (`fp_muladd` in
`src/a64_fp.c`: unpack, 128-bit exact sum, `round_pack`), once per lane.

Single precision now takes a host path when every operand that takes part is
a normal number or a zero (`fma32`, `fp_vec32_muladd`):

1. the product is computed in binary64, where it is exact (24 + 24 bits);
2. the addend is added in round-to-nearest and Knuth's TwoSum gives the
   exact error of that sum;
3. if the error is nonzero and the sum's last significand bit is 0, the sum
   moves one unit in the last place toward the error (round to odd);
4. one `cvtpd2ps` under the guest's MXCSR rounds to single in FPCR.RMode.

Rounding to odd at 53 bits and then to 24 bits in any mode equals rounding
the exact value once (Boldo and Melquiond; needs 53 >= 24 + 2). The odd
last bit also makes the conversion inexact exactly when the fused result
is, so the conversion's PE and OE are FPSR.IXC and OFC with no further
bookkeeping. The path covers FMLA and FMLS `.4S`/`.2S`, both by element,
the scalar by-element forms, and FMADD, FMSUB, FNMADD and FNMSUB on
singles, in the interpreter and therefore in the JIT's and AOT code's
helper calls. It uses SSE2 only.

It declines, leaving MXCSR as it was, and the software path runs unchanged
when an operand is a NaN, an infinity or a subnormal, when the
result is zero or below 2^-125 (ARM detects tininess before rounding and
SSE after, FZ flushes, and an exact zero takes its sign from the rounding
mode), or when FPCR has a bit set other than RMode, FZ and DN. A zero
operand with a nonzero result needs no rule of its own: the binary64 sum
is then the other term exactly. A vector
whose lanes are not all admitted is computed lane by lane, and each lane
is admitted or declined alone.

Without step 3 the path is wrong. `0xbc000008 * 0xc03ffff4 + 0x483bb5c8`
under round-to-nearest is `0x483bb5c9`; the binary64 sum lands on the
midpoint of two singles and a second rounding gives `0x483bb5ca`.
`0xde800000 * 0x5e800000 + 0x5e800000` under round-toward-zero is
`0xfd7fffff`; the binary64 sum absorbs the addend and converts to
`0xfd800000` with no inexact flag from the conversion.

The software path also no longer calls `nans()` unless `unpack` classified
an operand as a NaN; the call had no effect otherwise. This applies to half
and double precision as well.

Checks against native arm64. `make oracle` has an `fma` mode
(`tests/oracle/fma_cases.h`): every form above with distinct and aliased
registers, all sixteen RMode/FZ/DN settings, clear and preset FPSR, on
ordinary, arbitrary, near-tie, wide exponent gap, 53 to 57 bit sum,
overflowing, tiny, exactly cancelling and special operands, with one lane
in six from another class. For these records FPSR's cumulative flags must
match as well as the registers. 1,000,000 records matched in the
interpreter and through the JIT; 868,009 of them had only normal or zero
operands and results of at least 2^-125 in every lane, and for 68,427 of
those
`(float)((double)a * b + c)` differs from native. With step 3 removed,
9,699 of 100,000 records mismatched. `tests/guest/fma32.sh` runs 25
instruction forms on the same operand classes (675,000 instructions) and
requires the JIT, `--interp` and AOT runs to print native arm64's result
and FPSR hashes. `make oracle ORACLE_MODES="simd dp" ORACLE_N=1000000` had
no mismatch, and `make check` passed (`make test`, `oracle-asan` with the
`fma` mode, and the `TEST_SWITCHES` guest harness: 68 passed, 0 failed, 1
known failure in `guest/xinhab`, 8 expected skips).

Measured on the Apple silicon development host, where x86_64 hao runs
under Rosetta (not on Intel hardware), ns per instruction on normal
operands, JIT and AOT alike since both call the interpreter helper:

| Instruction | Before | After |
|---|---|---|
| FMLA/FMLS `.4S` | 128 | 22 |
| FMLA/FMLS `.4S` by element | 130 | 25 |
| FMADD/FMSUB single | 41 | 23 |

Remaining limitations:
- Each fused instruction is still a helper call from translated code; the
  path is not emitted inline.
- A rounding mode other than round-to-nearest needs two extra MXCSR writes
  per instruction (the sum must run in round-to-nearest). Under Rosetta
  `.4S` under RZ went from 167 to 86 ns rather than to 30 in the same run;
  the cost on Intel hardware has not been measured.
- A vector with a subnormal, infinite or NaN lane, or a tiny or zero
  result in a lane, is computed lane by lane. (`.4S` with one zero
  multiplicand lane and nonzero results is admitted whole: 159 to 23 ns.)
- Double precision stays in the software path: there is no wider host type
  to hold the product. It would need error-free transformations in
  binary64 itself (Dekker's product with Veltkamp splitting, TwoSum, and
  the Boldo-Melquiond three-addition scheme with an emulated round-to-odd
  addition), with guards for splitting overflow, an inexact low product
  part near the subnormal range, and separate handling of the directed
  modes.
- Half precision vectors keep `fp_vec16_muladd`, which admits a lane only
  when the exact sum fits binary64. The round-to-odd step would remove
  that bound (53 >= 11 + 2); the final rounding would stay in `fp_cvt`,
  since the profile has no half-precision conversion instruction. Scalar
  half fused operations are always computed in software.
- Not measured on Intel hardware or in an application.


## Guest calls of private-library functions that take a `va_list`

A guest `va_list` is a pointer to 8-byte stack slots; an x86_64 `va_list` is
a pointer to `{gp_offset, fp_offset, overflow_arg_area, reg_save_area}`. hao
converts one only for functions in its `va_list` tables (`src/interpose.c`
`vlists`): hand rows, plus `src/vlist_gen.inc`, which is generated from SDK
declarations. A private library's functions are in neither, so a guest call
of CoreUtils `NSErrorV(domain, code, format, va_list)` handed the native
function the guest pointer itself. CoreUtils' `PrintFCoreVAList` then read
the guest's slots as an x86_64 `va_list` and faulted at the first pointer
conversion (`%@` through `CFGetTypeID`, `%s`).

The hand rows now include the 26 CoreUtils exports that take a `va_list`.
They were found by register flow in the arm64e code: an export whose argument
register reaches the `va_list` position of `PrintFCoreVAList` (index 2), or
of `CFPropertyListBuildFormatted` and `VSNScanF`, which read it in place
(`ldr`, `add #8`, `str` on its spill slot), followed to a fixpoint. Variadic
functions, which make their own `va_list` with `va_start`, are excluded. In
the 26B131 x86_64 cache all 26 names are exported, and only by CoreUtils.
The conversion is the one the SDK `v*` functions use: register save areas
marked exhausted, overflow area at the guest's slots, so integer and floating
arguments are read in the guest's order with no format typing.

`tests/guest/cuvalist.sh` has a guest variadic wrapper hand its `va_list` to
`VSNPrintF`, `VASPrintF`, `NSPrintV` and `NSErrorV` with string, integer,
double, `%.6a` and `%@` conversions. Before the change it crashed with
SIGSEGV under JIT, `--interp` and AOT; after it, all 6 lines equal native
arm64.

`src/vlist_gen.inc` now also has rows derived from code: both slices of the
26B131 caches, x86_64 `__va_list_tag` reads, arm64e `va_arg` loads and
argument forwarding to a fixpoint, kept only when the two slices agree on the
index. Without seeds, that derivation reproduced 55 (x86_64) and 30 (arm64e)
of the 76 SDK and hand rows and never gave a wrong index. It confirms the
CoreUtils hand rows: 23 from both slices, `CFCreateV` and
`CFPropertyListCreateFormattedVAList` from x86_64 only, and none contradicted.
`VSNScanF`, a scanf consumer, is outside what it can derive. Over all 17,745
C/C++ guest imports of 26B131, the only `va_list` parameters beyond the SDK
and hand rows are `NSErrorV`, `NSErrorNestedV` (hand rows) and `_ISDVLog`
(SyncServices, index 3, the one new generated row).

Remaining limitations:
- The derivation does not cover a `va_list` passed as the seventh or a later
  argument, scanf consumers, or ObjC methods (those go through their type
  encoding in `src/objc.c`). A private function of those kinds needs a hand
  row.
- A function with a floating-point argument before its `va_list`, or with more
  than five integer arguments before it, cannot go through `call_with_va`. No
  CoreUtils row has either.

## Guest calls of native Optional, tuple and array witnesses

A nested Optional whose outer nil is a pointer's extra inhabitant (`C??`,
`[Int]??`, `AnyObject??`, `(any P)??`, the storage of a `lazy var` of
Optional type) is written by guest code inline with the arm64 encoding:
inhabitant 1 is the value 1. libswiftCore's x86_64 witnesses read 2. This
was measured in native programs of both architectures; for an existential
the inhabitants are in its type-metadata word (opaque) or object word
(class-bound). Guest code that is not specialized hands such values to those
witnesses: generic code, and outlined copies and destroys through metadata
from a mangled name, such as a class's ivar destroyer destroying a lazy
property that was never read. The x86_64 witness takes the outer nil for a
live payload and copies or destroys garbage. For `(any P)??` that is a
destroy of an existential whose type is 1, a fault at 0xfffffffffffffff9.

hao now recognizes a guest call of these native witnesses:
- the Optional value witnesses, which every Optional instantiation shares
  (`$sSqwCP`, `wxx`, `wcp`, `wca`, `wtk`, `wta`, `wet`, `wst`, `wug`, `wup`,
  `wui`);
- the tuple witnesses, a few `tuple_*` sets chosen by layout;
- the runtime's array operations (`swift_arrayDestroy`,
  `swift_arrayInitWithCopy`, `...WithTake*`, `swift_arrayAssign*`), which
  inlined Array code calls.

The witness addresses are read at image registration from Optional and tuple
instantiations of the converted shapes, and the array operations by name. A
call counts only when its metadata argument's own table holds the called
witness in that slot. For a type that can hold such an outer nil (cached per
metadata), the witness gets each outer nil in its own encoding:
`receiver_empty_bits` asks the type's witnesses, so a guest payload type's
arm64 inhabitants are kept. A borrowed source is converted in a scratch copy
and an owned one in place. A value the witness produces gets back its
source's bits at those offsets, so a value keeps the encoding it came with:
a native-made value passing through guest generic code stays x86_64. A nil
made from nothing (storing or injecting a tag) gets the guest's encoding
where guest code can read it inline: every struct on the path to its word
is defined by a guest image (or is `Array` or `String`). Inside a struct of
a native module, which may be resilient (libswiftos `OSSignposter`), the
nil keeps the native encoding. Only native witnesses read it there,
including those native code reaches while handling an enclosing guest
struct, and no conversion sees those. Converting it anyway once made a
guest `Optional` of a struct whose first field is `OSSignposter?` read as
present when nil: the native tag read took the guest's 1 for a payload,
copies duplicated garbage, and a force unwrap in the guest trapped.
Only the outer-nil words change, never a live payload. A value the walk
cannot prove is handed over unchanged, as before. The walk follows
existential payloads (opaque and class-bound; not the Error box or other
special protocols), Swift class references, and Arrays of any element, but
only for these calls. Published values (below) keep their narrower rules,
because there native code takes the value.

`tests/guest/optwitness.sh` has unspecialized generic guest code copy,
destroy, assign and test values through these witnesses: nested Optionals
of an existential, a class-bound existential, a Swift class, `AnyObject` and
`[Int]`, in each of nil, some(nil) and some(some) form, plus a class whose
lazy `(any P)?` property is destroyed unread, read as nil, and read as a
value. Before the change it crashed under JIT, `--interp` and AOT at the
first copy; after it, all 94 lines equal native arm64. It now also copies,
assigns, tests and drops nil, full and empty values of `Optional` of such a
guest struct, and makes a nil one through generic code. Before the narrower
nil rule that case stopped with "Unexpectedly found nil" in all three modes;
now all 101 lines equal native arm64.

Remaining limitations:
- Native code that reads such a value itself, rather than a witness the guest
  calls, still sees the guest's encoding. Examples: native generic code (an
  Array element read through the non-inlined `Array` subscript, which copies
  it with the witness natively) and `swift_dynamicCast` out of `Any`. Memory
  that guest inline code and native code both read can hold only one
  encoding. That is the open part of `guest/xinhab`, which stays `xfail`.
- A native struct counts as guest-readable only when `src/frozenstruct_gen.inc`
  lists it: a table generated from the SDK interfaces' `@frozen` and
  `@_fixed_layout` declarations, joined against the 26B131 x86_64 cache and
  matched at run time by qualified nominal name, since freezing is
  permanent ABI. A frozen struct the table misses (for example in a private
  module without an interface) is treated as resilient and keeps the native
  encoding. Before the table, every native struct was: a nil made through the
  shared Optional witnesses inside `CollectionOfOne<AnyObject>??` read as
  present in guest code.
- Witnesses of native generic structs and enums other than Optional (each
  type has its own) are not recognized: a nil a generic struct's own native
  witness makes (`CollectionOfOne<AnyObject?>?` from unspecialized generic
  code) keeps the native encoding, whatever the frozen table says. Neither are value boxes (an `Any`
  holding such a value is destroyed by the box's witness).
- Two consumed inhabitants are the bound, as for published values: a third
  nesting level is handed over unchanged.

## `Optional<AnyObject>` in a published Swift value

A guest SwiftUI view value that held an `NSNotificationCenter.Publisher`
was refused at publication, which ended the process: `Swift value boundary
refused: result publication has unresolved active representation (...
type=Swift.Optional<Swift.AnyObject>)`. A view value returned by guest code
(`View.body`, an associated result) that holds an enum whose empty cases sit
in a pointer's extra inhabitants is walked whole, and the walk reached the
publisher's `object`, which is `Optional<AnyObject>`. `common_leaf_value`
admitted an `Optional` of a Swift class, of an imported ObjC class, or nil of
a one-witness class existential, but not `AnyObject`.

Native Swift programs on arm64 and x86_64 give the same `AnyObject`:
existential kind 0x303, flags 0, no protocols, the `$sBOWV`
(Builtin.UnknownObject) witnesses (size 8, flags 0x10007, xi 0x7fffffff).
Its first `Optional` has size 8, flags 0x210007 and nil 0 on both. hao now
admits exactly that first `Optional`, with the value rule of imported ObjC
references: nil, or a word the witnesses' own tag function calls a live
value, tagged pointers included. `Optional<Optional<AnyObject>>` stays
refused, because its second nil is an extra inhabitant that is 1 on arm64
and 2 on x86_64.

`tests/host/swiftvalue.sh` walks native x86_64 values with an `NSObject??`
field (walked, with extra inhabitants to patch) next to an
`Optional<AnyObject>` holding nil, an NSObject, a Swift object or a tagged
NSNumber. Before the change every case was refused at
`Optional<AnyObject>`, the publication refusal above. After it they are
walked with the patches of an `Optional<NSObject>` control.

Two more first `Optional`s were refused in values crossing the boundary:

- The Optional of an enum with no payload. It is one byte: a case is 0..n-1
  and nil is n, measured identical on both architectures. The byte rule
  needed 9 bytes or more. Now the first Optional of a payload-free enum of up
  to 255 cases is admitted when its byte is at most n.
- `Optional<Foundation.URL>`. URL is a struct whose only
  field is `any _URLProtocol`, a class-bound existential with one witness
  table: 16 bytes, measured identical on both architectures, nil is a zero
  object word. Only nil of a bare such existential had been admitted, and
  only with flags exactly 0x210007, while URL's Optional also has
  0x2000000 (addressable for dependencies), which changes no layout. Now
  that wrapper is admitted with a live value too. The object word must pass
  the existential's own tag function and the witness table must lie in no
  guest image, since a guest conformance's table is arm64 code. A bare
  existential stays nil-only, because a weak field reports the same type and
  its nonzero storage is not a strong pointer.

`tests/host/swiftvalue.sh` covers both (nil and live values beside the
`NSObject??` field): refused before, walked after. Its URLs all have native
witness tables, so the refusal of a guest witness table is not exercised
there.

## `%a` slots of variadics whose format family is assumed

A guest call of CoreUtils' `LogPrintF` with a `%.6a` conversion (an
Ethernet address: `LogPrintF(cat, func, level, "%.6a\n", bytes)`) crashed
with SIGSEGV in CoreUtils' `PrintFCoreVAList`. `LogPrintF` has no SDK format
attribute, so `src/varsniff.c` typed its slots by assuming the last fixed
argument is a printf format. printf's `%a` takes a double, so the pointer
went to xmm0. CoreUtils reads `%a` as a pointer to an address and took r8,
which held 0.

Native arm64 CoreUtils on the development host (27.2, 26B123), read through
`VSNPrintF` with marker slots, gives these rules. `%a` takes one pointer with
any precision or flag: `.4` is IPv4, `.6` Ethernet, `.8` 8 bytes, `.16`
IPv6, `##` a sockaddr, and a bare `%a` prints an error. `%A`, `%D`, `%O` and
the `L` and `q` modifiers are unknown and take no argument. `v` is a
NumVersion conversion, not a modifier. `%m` takes one integer, `%H` takes a
pointer and two integers, and `%N` takes nothing. `%e %f %g %E %F %G` take
doubles.

A `%a`/`%A` slot of a function without an SDK format attribute is typed by
that function's formatter family, from `src/fmtfamily_gen.inc` (generated
from the x86_64 code: which formatter the function's arguments reach, to a
fixpoint; checked against every SDK-attributed function, none wrong):
CoreUtils' PrintF takes a pointer, printf and CFString formats a double. A
function the table does not list is assumed to be printf. An earlier rule
typed such a slot by its value (a user pointer reads as zero or a positive
subnormal double); no rule of that kind can tell a NULL pointer from 0.0 or
an address from a subnormal, and it misrouted both a bare `%a` of NULL to
CoreUtils and a subnormal to printf. Formats with an SDK attribute are typed
as before. `tests/guest/varsniff.sh` checks printf `%a` (zero, precision,
`#`, `%A`, the smallest subnormal and the minimum normal) and CoreUtils
`SNPrintF` (`%.6a`, `%.4a`, `%##a`, a NULL `%.6a` and a bare `%a` of NULL,
each followed by integer, string and double slots) against native arm64.

Remaining limitations:

- A CoreUtils-family function the table misses (not exported, a family
  reached only through a format string such as CoreUtils' `%V`, a va_list
  passed on the stack) is typed as printf: its `%a` pointer goes to an XMM
  register and later slots read one register off.
- CoreUtils conversions outside printf's set (`%m`, `%H`, `%N`, `%{...}`,
  `%v`) still make the typer give up, and every slot then goes to a GPR. A
  real `%f` in such a format prints a wrong value but cannot crash. CoreUtils'
  `%D`, `%O` and `%q` take no argument but are typed as one slot.

## Signal callback reentry during Objective-C enum initialization

The intermittent `tests/guest/uresume.sh` `alarmnx` SIGILL was a runtime
signal-reentrancy defect. Every untyped native-to-guest callback calls
`hao_objc_enum_args`, which lazily registers selectors with `pthread_once`.
A SIGALRM arriving during that first initialization ran a guest handler
that made another native-to-guest callback. That callback entered the same
unfinished `pthread_once` on the same thread.

A JIT diagnostic run with `HAO_SIGRETURN_KERNEL=1` on the Apple Silicon
development host captured RIP `0x7ff80af03d4b`, resolving to
`libsystem_platform.dylib`'s `_os_once_gate_recursive_abort+0x17`, with
runtime depth 2, signal depth 2, signal nesting 1 and critical depth 0.
The stack contained both `hao_objc_enum_args` calls and the interrupted
outer `enum_init`. This was not the `ud2` after a failed `__sigreturn`.
The guest PC was the callback target (file VM address `0x1000015e0` in
that build), not an instruction fetched from an incorrectly located JIT
block. Temporary diagnostic instrumentation was removed after capture.

`hao_objc_enum_args` now surrounds the complete `pthread_once` call with
hao's existing critical-section protocol. An arriving asynchronous guest
signal is deferred until `pthread_once` has published completion, then
redelivered by `hao_crit_leave`. Guarding only the initializer body would
leave a reentry window before `pthread_once` publishes completion. The
change applies to every caller of the enum bridge, independent of the
program, execution engine and signal-return route.

On 10 November 2026, unmodified `1d230dc` with `TEST_SWITCHES=1` failed
51 of 1,200 fresh processes, in two batches of 100 per route. Every captured
failure had the recursive-once callback stack, including the interpreter
failure. With the fix, all six routes passed 500/500, or 0 failures in
3,000 fresh processes. The native ARM64 reference passed 100/100 fresh
processes.

| Engine | User resume, before | Kernel resume, before | User resume, after | Kernel resume, after |
|---|---:|---:|---:|---:|
| JIT | 20/200 | 19/200 | 0/500 | 0/500 |
| Interpreter | 0/200 | 1/200 | 0/500 | 0/500 |
| AOT | 4/200 | 7/200 | 0/500 | 0/500 |

Additional unmodified-runtime controls used JIT with the same fixture:

| Control | User resume failures | Kernel resume failures |
|---|---:|---:|
| `HAO_NO_CHAIN=1` | 1/100 | 1/100 |
| `HAO_JIT_HELPERS_ONLY=1` | 0/100 | 0/100 |
| Both `39d42d1` and `09a3420` reverted in a scratch build | 0/100 | 0/100 |

The no-chain failures had the same recursive-once stack. Zero failures in
these smaller controls are timing observations, not evidence that the
controls fix the defect: the second unmodified JIT batch also passed
100/100 on each route after the first batch failed 39/200. The captured
once recursion and the interpreter failure identify a cause independent
of either optimization.

Each AOT build uses its own newly generated cache; the fixed build's
`--stats` check reported `aot_blocks=256 aot_rejected=0`. JIT and
interpreter runs use `HAO_NO_AOT=1`. User and kernel routes set
`HAO_SIGRETURN_USER=1` and `HAO_SIGRETURN_KERNEL=1`, respectively.
Each guest process has a 60-second kill timeout; stdout and exit status
are compared with the native ARM64 `alarmnx` reference. The tests retain
their 100-microsecond timer and require at least 5,000 checked callbacks,
50 alarms and zero callback errors. No retries or relaxed expectations
are used.

`make check` passed: `make test`, `oracle-asan`, and the `TEST_SWITCHES`
guest harness (64 passed, 0 failed, 1 known failure in `guest/xinhab`,
8 expected skips, 73 scripts total). The complete `guest/uresume` script
passed, including its state, mask, nesting, exception, thread and
alternate-stack cases and signal-return counters. The gate's host fork
stress also passed 300/300 without hangs or errors; it does not resolve
the historical fork issue below.

The host was macOS 27.2 (26B123) on ARM64, running x86_64 hao through
Rosetta. The captured recursive-once ordering is a runtime bug that also
applies on Intel; execution on an
Intel kernel remains necessary to confirm the fix there. Rosetta's
separate restriction on abandoning nested signal frames still applies,
as described in `src/signals.c`. This result does not establish that all
signal-handler paths are reentrant or change the concurrent fallback in
`hao_jit_locate`. Application and release-image acceptance remain separate.

## Intermittent host fork stress failure

`tests/guest/fork.sh`'s host `test_fork all` has historical SIGSEGV/SIGBUS
failures. Reports from November 2026 include JIT emission
(`x_mov_ld`/`exit_indirect`) and bridge binding (`type_at`) stacks.
`test_fork` does not install hao's guest signal handlers, and those stacks
do not show the recursive enum initialization above. One more SIGSEGV
came from bridge binding (`labels_at` under `type_at`, a `w_bridge`
thread) in a `make test` run on 11 November; six reruns right after passed.
No shared cause has been established; the fork failure remains open.

## Repeated per-call work on guest -> native calls

Every guest branch to a native address went through the same
classification again: `run_loop` asked whether the address was a trap,
guest code (`hao_image_for_code`: the last-image hint, then the range
snapshot), a native entry of a guest function (three thunk tables) or a
shared-cache stub or island (`hao_dsc_redirect`, which takes `dsc_lock` for
an address in the cache), and `native_step` looked the plan up again
(`hao_bridge_plan_for_call`). None of that depends on the call, only on the
address and on state that changes rarely. After the native function
returned, `hao_jit_run_block` probed the whole translation table for the
return address, which no guest `ret` puts in the thread's jump cache.

Now each thread keeps a direct-mapped table in its `hao_thread`
(`src/runtime.c`, native-target cache; 256 entries, 10 KB of the thread's
state mapping): branch address -> native target, the `x1` a redirected
`objc_msgSend` stub sets, and the plan. An entry is
used only while the process-wide generation `hao_ncache_gen` (`hao/macho.h`)
is the one its classification started in; image registration and release
and every plan-table insert or replacement (`tab_put`) increment it after the
change is visible. Traps, guest entries, cache addresses the redirect could
not resolve, hao's own code and plans whose lookup still has side effects
(an ObjC probe plan with probes left, no plan) are never stored. Everything
after the plan still runs per call (selector recheck, interposer redirect,
REFUSED, counting with `HAO_BRIDGE_STATS`/`HAO_BRIDGE_CHECK`, Swift value
and witness hooks, the argument scan), so routes, diagnostics and counters
are unchanged. Also: the dispatcher reads and fills the thread's jump cache
for the block it enters; the argument scan answers "in no guest image" for
a register value with one range search instead of two lookups;
`hao_native_call` copies the usual 16 stack words with vector moves; the
ObjC enum-argument check skips its `pthread_once` critical section once
initialised.

Measured on the development host, where hao itself runs under Rosetta
(ns per call, minimum of 7 interleaved runs of 10^6 calls each, before ->
after, JIT / AOT): EXACT unit-typed function 36.9 -> 26.2 / 36.6 -> 24.0;
TYPED narrow result 39.7 -> 26.8 / 39.1 -> 26.1; untyped (`strlen`)
65.0 -> 49.8 / 60.0 -> 49.2; `memcmp` 92.3 -> 73.1 / 90.1 -> 74.3;
`CFStringGetLength` 79.9 -> 69.9 / 79.5 -> 68.2; a function pointer from
`dlsym` 72.2 -> 62.0 / 71.1 -> 57.9; `objc_msgSend` to a native method
394 -> 378 / 405 -> 372 (native arm64: 1-3 ns). In `sample` profiles of
the `strlen` loop, `hao_image_for_code` fell from 8.9% to 1.9% of samples,
and `hao_bridge_plan_for_call` (2.2%), `hao_dsc_redirect` (0.8%), the
thunk-table lookups (2.3%) and `hao_is_guest_function` (6.4% with the
lookups it made) no longer appear. `tests/guest/nccache.sh` checks that a plan typed after five
threads cached the untyped one reaches all of them (it fails, 1000 of 1000
wrong results per thread, with the generation increment left out of
`tab_put`).

Remaining limitations:
- What is left per call is the JIT exit and re-entry, the exception guard,
  `hao_native_call`, and for every untyped or masked slot the argument scan,
  which reads memory the arguments point to and cannot be remembered by
  target. A native call stays about 20-25 times its native cost under
  Rosetta.
- `objc_msgSend` to a native method calls `class_getInstanceMethod` for the
  encoding on every message (about two thirds of its 380 ns here); its
  result is not cached, since a method list can change without a hook hao
  sees.
- With `HAO_BRIDGE_STATS` or `HAO_BRIDGE_CHECK` each call still increments its
  plan's shared counter atomically, which threads calling one function
  contend on.
- Not measured on Intel hardware or with the target's interface units; the
  share each change has there is not established.
