# Runtime issues and measured scope

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
- `FRINT*`, the non-truncating `FCVT*` modes, fixed-point conversions,
  `FCVTZU` and `UCVTF` with large lanes, the double packed conversions, the
  other compares with zero and packed max/min still go through the helper;
  its dispatch is then most of their cost.
- Inline SSE arithmetic that predates this change (FADD, FSUB, FMUL, FDIV,
  FSQRT, FCVT single<->double, FCMP) differs from A64 in FPSR under FZ: a
  flushed denormal operand does not raise IDC, and a result flushed to zero
  raises IXC together with UFC where A64 raises UFC alone. Values agree. The
  interpreter (`--interp`) is exact. `tests/guest/fpscalar.c` runs those
  instructions with FZ clear.
- FPCR.AH, FIZ and NEP are not modelled; the test leaves them clear.
- The packed tests for denormal lanes also send a lane equal to the minimum
  normal number to the helper (exact, slower).

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
- A frozen struct of a native module other than `Array` and `String`,
  which guest code could read inline, gets no guest-encoded nil when one is
  made inside it. The descriptor does not say whether a struct is frozen.
- Witnesses of native generic structs and enums other than Optional (each
  type has its own) are not recognized. Neither are value boxes (an `Any`
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

When the family is only assumed, a `%a`/`%A` slot without `L` now goes by its
value. An x86_64 user pointer is below 2^47, which read as a double is zero or
a positive subnormal, and no other double is. Those values go to a GPR and
everything else to an XMM register. Zero is both NULL and 0.0. It counts as an
address when the conversion has a `#` flag or a precision, the forms CoreUtils
needs, and as a double otherwise. Formats with an SDK attribute are typed as
before. `tests/guest/varsniff.sh` now checks printf `%a` (zero, precision,
`#`, `%A`) and CoreUtils `SNPrintF` (`%.6a`, `%.4a`, `%##a`, a NULL `%.6a`
followed by integer, string and double slots) against native arm64. Before
the fix the CoreUtils case crashed in `PrintFCoreVAList` under the JIT and
`--interp`.

Remaining limitations:

- printf `%#a` or `%.Na` of exactly 0.0, or `%a` of a positive subnormal,
  in an unattributed variadic followed by slots of the other class: those
  later slots read one register off.
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
