# Tests

```sh
make test          # quick gate: unit, oracle, aot, bridge/async/dyld/mulhigh, forktest, jitlocktest, check_x86
make test-guest    # every other script that runs unattended here (tests/check.sh)
make check         # the gate for runtime changes: test, oracle-asan, test-guest
make oracle-asan   # oracle checks with the translator under ASan + UBSan
```

`tests/check.sh` runs the entries of `tests/check.list` in parallel (`J`
jobs, `make test-guest` passes `CHECK_J`, default 8), each in its own process
group with a timeout and a private `mktemp -d` output directory passed as
`OUT`. It prints one line per entry:

| Line | Meaning |
|---|---|
| `PASS` | exit 0, no `SKIP` line |
| `FAIL` | non-zero exit, or the entry's timeout |
| `SKIP` | the script printed `SKIP <reason>` lines (exit 0, 2 or 77); each reason is matched against `tests/expected-skips.txt`, and one that is not listed is shown as `UNEXPECTED` and fails the run |
| `XFAIL` | an entry marked `xfail` in `tests/check.list` failed, as recorded below |
| `XPASS` | an `xfail` entry passed: fails the run until the mark is removed |

The last line counts each kind. The exit status is 1 on any `FAIL`,
`XPASS` or unexpected skip. `KEEP=1` keeps the logs (`<dir>/<name>/log`);
they are kept anyway when the run fails. `EXPECTED_SKIPS=file` adds
host-specific expected skips. `tests/check.sh NAME...` runs some entries.

Without `OUT`, every script now works in a new `mktemp -d -t hao-NAME`
directory instead of a fixed `$TMPDIR/hao-NAME`; callers that pass `OUT` are
unchanged.

The half-precision `guest/halfops` and `guest/fma16` matrices include all 64
RMode/FZ/DN/FZ16/AHP settings, with clear and sticky FPSR. Halfops also checks
scalar FCVT H/S/D, vector FCVTN/FCVTN2/FCVTL/FCVTL2, and widening arithmetic
at half subnormal, alternative-format, overflow and NaN boundaries. Native,
interpreter, JIT, helpers-only and AOT outputs must match. Random SIMD, block,
known and FMA oracle records also draw FZ16 and AHP; AFP bits remain excluded.

## What you need

Everything in `make test` and `make check` is self-contained apart from the
host: an Apple silicon Mac with Rosetta (the arm64 references run natively,
hao's x86_64 code under Rosetta), Xcode's clang and swiftc with arm64e,
Python 3.14 and `timeout` in `PATH`. No test needs root or the network, and
none launches a GUI application, app extension or XPC service.

Some cases use inputs that are not in this repository. Without them those
cases print a `SKIP` line that `tests/expected-skips.txt` accepts, so the run
still passes:

| Input | Used by | What to supply |
|---|---|---|
| `HAO_FS` | `guest/cli` (Apple-signed `ls`, trusted cdhash list), `tools` (MenuBarAgent bundle) | the path of a mounted macOS 27 System volume (read-only is enough) |
| `KC` | `tools` stamp cases | a BootKernelExtensions.kc of the target kernel |
| `PROFILE` | `host/sigpolicy` profile cases | a sandbox profile to check |

## Inventory

Measured on an Apple silicon development host (macOS 27.2, 16 cores) with
`HAO_FS` set. Times are wall-clock seconds; `test-guest` times are under
`J=8` parallel load.

`make test` (173 s in all; each target alone):

| Target | Result | Time | Needs |
|---|---|---|---|
| `unit` | PASS | 3 | (now also `tests/aot/jit_vector.c`, below) |
| `oracle` | PASS | 153 | |
| `aot` | PASS | 6 | |
| `bridgetest` | PASS | <1 | |
| `async-contract-test` | PASS | 4 | python3.14 |
| `legacy-dyld-test` | PASS | 1 | python3.14 |
| `indexed-mulhigh-test` | PASS | <1 | python3.14 |
| `forktest` (added to `make test`) | PASS | 7 | |
| `jitlocktest` (added to `make test`) | PASS | 37 | `/usr/bin/awk` copies |
| `tools/check_x86.sh` | PASS | 1 | |

`make test-guest` (about 150 s; 77 entries: 74 PASS, 1 XFAIL, 2 expected SKIP with `HAO_FS` set, 3 without it):

| Entry | Expected | Time | Needs / skip reason |
|---|---|---|---|
| guest/aggspi6 | PASS | 3 | `node-query`: a 24-byte client context whose retain/release (guest functions) the native callee calls; `mr-thumbs`: a 2064-byte record first and a guest block the callee invokes; each crashes without its `src/aggspi_gen.inc` row; `si-path-filter`: a guest callback returning a 28-byte struct indirectly, kept by a native `SIPathFilterCreate` and called later (needs its `src/callbacks.c` binding) |
| guest/atoken | PASS | 1 | |
| guest/bitfields | PASS | 2 | |
| guest/blocks2 | PASS | 2 | |
| guest/bridgefix | PASS | 2 | |
| guest/bridgeunit | PASS | 3 | |
| guest/callback-routes | PASS | 2 | |
| guest/callbacks | PASS | 10 | |
| guest/cfnull | PASS | 1 | |
| guest/diagtime | PASS | 1 | |
| guest/faultstack | PASS | 20 | a fatal native fault at each depth of nested native->guest callbacks on a 64 KB thread, until the stack overflows first: every run reports (`host fault`), the fault itself first unless its signal frame did not fit (`_sigtramp+0`). Before the report had its own stack, the depth where the frame fits but the report does not ended with no report |
| guest/fetchgiveup | PASS | 2 | native qsort calls a comparator inside a PROT_NONE page: the fetch fault is given up and must fault again, ending as natively (status, stdout) with the give-up line at the target. Before, the give-up read the bytes before the faulting rip and died of that read inside the handler, 2 bytes off and with no line |
| guest/logclient | PASS | 2 | `os_log_set_client_type` first thing in main in a program linked with GSS.framework: natively it traps when CoreFoundation's initializer has started libtrace (`__CF_USER_TEXT_ENCODING` unset, the control) and runs when it has not; under hao (JIT, `--interp`, AOT, and with `HAO_NX_STATS`' thread) the request is not applied and the program runs |
| guest/chkstk | PASS | 1 | |
| guest/cli | PASS | 3 | two cases need `HAO_FS` (expected SKIP without it); `sandbox-exec` |
| guest/crypto | PASS | 1 | |
| guest/cuvalist | PASS | 2 | private CoreUtils on the host (`VSNPrintF`, `NSErrorV`, ...) |
| guest/deepcallback | PASS | 1 | guest -> native -> guest recursion (qsort's comparator sorting again) 250 levels deep on a 512 KB thread: hao's native frames per level must fit where native arm64's do |
| guest/cxxabi | PASS | 10 | |
| guest/cxxbool | PASS | 3 | C++ functions with ObjC BOOL parameters in a native library: the guest imports `..b..` (bool on arm64), the x86_64 slice exports `..a..` (signed char) |
| guest/datacall | PASS | 1 | |
| guest/dsc | PASS | 3 | the host's arm64e shared cache |
| guest/dscswift | PASS | 10 | the host's arm64e shared cache |
| guest/dylibs | PASS | 4 | |
| guest/eh | PASS | 19 | |
| guest/aotdefault | PASS | 3 | `hao --aot` with no FILE: the default set from aot-list, else stub-identities (HAO_AOT_LIST_DIR); missing files counted; lists others can write not used; [N/M] lines; valid caches skipped; `--check` valid/missing/stale; `--auto` waits for free space (HAO_AOT_FREE_MB) |
| guest/dlrpath | PASS | 3 | dlopen of @rpath, @executable_path and @loader_path names from an app bundle: resolved from the calling guest image; an ARM-only dylib loads as a guest image, a universal one natively |
| guest/fenv | PASS | 1 | |
| guest/hostcode | PASS | 2 | calls through dlsym pointers: the host-code check answers from the image (0 kernel walks); a pointer to native data is still refused |
| guest/fma32 | PASS | 2 | |
| guest/fma64 | PASS | 8 | double FMADD family, FMLA/FMLS .2D and by element against native: JIT, `--interp`, helpers-only, AOT |
| guest/fmtpos | PASS | 2 | native variadics with six to eight fixed integer arguments |
| guest/footprint | PASS | 59 | `footprint(1)` |
| guest/fork | PASS | 45 | |
| guest/forkabort | PASS | 1 | a fork child aborts after getpid and getppid: with `HAO_CRASH_TRACE=1` its abort report (JIT, `--interp`) names the fork and lists the thread's last native calls (fork, getpid, getppid, abort) with their lr. Before, neither line was there |
| guest/fpscalar | PASS | 55 | |
| guest/fullcache | PASS | 6 | |
| guest/fwbundle | PASS | 20 | a guest framework finds its bundle by identifier (`bundleWithIdentifier:` then a resource), by class (plain, resilient-ancestry, generic and pure Swift classes) and in `Bundle.allFrameworks`: equal to native arm64 under JIT, `--interp` and AOT. Without the framework's CFBundle the identifier lookup is nil (the control) |
| guest/gss | PASS | 1 | |
| guest/heapprobe | PASS | 1 | |
| guest/hfasret | PASS | 2 | |
| guest/identity | PASS | 5 | |
| guest/indirect-x8 | PASS | 1 | |
| guest/keypath | PASS | 2 | |
| guest/libval | PASS | 16 | |
| guest/lvswitches | PASS | 7 | the switches that add code or trust (`HAO_CACHE`, `HAO_GUEST_ROOT`, `HAO_IF_DIR`, `HAO_ASYNC_CONTRACT_LIBRARY`, `HAO_TRUST_LIST`) and `HAO_NO_LIBVAL`, set in an open-tier process: a program that requires library validation (hardened runtime in the CLI route, the library flag embedded) names each as ignored and does not load `HAO_IF_DIR`'s thunk library, whose initializer leaves a marker; an ad hoc program loads it (the control). Before, the hardened program loaded it. The library program's stub with its ARM slice replaced or corrupted ends with status 126 before the thunk library loads (before, its initializer ran first) |
| guest/msgfnptr | PASS | 4 | |
| guest/narrowret | PASS | 4 | |
| guest/nativesig | PASS | 25 | |
| guest/nccache | PASS | 3 | |
| guest/nojit | PASS | 5 | |
| guest/objcimp | PASS | 4 | |
| guest/objcva | PASS | 3 | |
| guest/optwitness | PASS | 1 | guest calls of native Optional, tuple and array witnesses on nested Optionals, also beside guest multi-payload enums (tag byte with several empty cases, spare-bit tags decoded from the image's layout record, Optionals over 17-byte spare-bit enums whose nil is one of the enum's inhabitants, and single-payload enums whose empty cases are their payload's inhabitants), and `Data??` whose nils are translated between the architectures' spare-bit encodings; see below. Its arrays of nested Optionals are admitted as carriers, converted at their destruction; no refusal (checked) |
| guest/overlaynil | PASS | 1 | a guest closure native SwiftUI calls (`View.overlay`'s content) returns an Optional whose nil hao's Optional witness calls left in the guest's encoding; the callback's return gives it the native encoding back (ControlCenter's Sound module); and a specialized body that inlines overlay copies `_OverlayModifier<P?>` through SwiftUI's own witnesses for the frozen struct. A content Optional whose nil is a third pointer inhabitant is refused by default (checked); the engines run with `HAO_SWIFT_VALUE_PASSTHROUGH=1` |
| guest/svpolicy | PASS | 1 | the Swift value boundary policy: tuples of a `C??` outer nil copied through native tuple witnesses beside a field of each pass class (named under `HAO_BRIDGE_CHECK`) and an `Any` holding a `C??` nil are converted, equal to native arm64; refused (refusal line, abort): a `C??` beside an `InlineArray` (with `HAO_SWIFT_VALUE_PASSTHROUGH=1` it goes on, sibling nil converted), an `InlineArray` beside a class reference only, a 257-element tuple, an AsyncStream's `next()` returning such a value, an `Any` whose boxed value holds a `C??` nil, a child task with a `C??` result (at its creation), a struct built without reflection metadata beside a `C??` (under `HAO_SWIFT_VALUE_STRICT=1`; by default the `InlineArray` case is reported and goes on, equal native, and the boxed `Any` is still refused); a native frozen enum the guest built is examined; an array of `C??`, a generic captured `C??`, `[Any]` storage and a box of `Any` holding guest-written `C??` nils, destroyed by the runtime, equal native (converted at destruction) |
| guest/svcarriers | PASS | 1 | extra inhabitants certified by ABI rule, through native tuple witnesses beside a `C??` nil: an existential metatype with a witness table (`any P.Type`, k against 2k, converted), a trailing Bool's and a payload-free enum's byte values, and SwiftUI's `ToolbarItemGroup` (its Bool); equal native arm64 under JIT, `--interp` and AOT (refused before these rules) |
| guest/carrierdestroy | PASS | 1 | storage of element types with a nil whose encoding differs (`Data?`, `C??`, layout-string structs holding them, one of 241 fields; the runtime lays out those with a UUID), arrays held by NSMutableArray and generic boxes, destroyed last by native code; equal native arm64 under JIT, `--interp` and AOT (refused when made before) |
| guest/sealedstorage | PASS | 1 | array storage of `Any` holding `Data?` and `C??` nils, its `_ContiguousArrayStorage` metadata in the guest's sealed `__DATA_CONST` (hand-laid in `sealedstorage.s`: swiftc prespecializes no stdlib class), destroyed by native code; equal native arm64 under JIT, `--interp` and AOT (refused when made before) |
| guest/svfacts | PASS | 1 | the walk's per-type facts from a fixture (test switch `HAO_SWIFT_WALK_FACTS`, a TEST_SWITCHES build), tuples beside a `C??` nil through native tuple witnesses: a SAME row passes an `InlineArray` the rules refuse, a CONVERT row's masked case images convert the `C??` nil, a REFUSE row refuses an `Int?` the rules certify; `HAO_SWIFT_VALUE_AUDIT=1`: the refused value goes on, equal to native, with one `hao-swift-audit` record (table key, chain, caller image UUID and offset, no address or path); the release `build/hao` ignores the audit, says so once and refuses |
| guest/svguest | PASS | 1 | guest-defined types' rows by identity (test switch `HAO_SWIFT_GUEST_FACTS`, a TEST_SWITCHES build; the fixture written from the built binary's LC_UUID, descriptor offsets and both architectures' measured layouts and case images): runtime-laid multi-payload enums (SAME; CONVERT with a `Data?` payload) a struct of `Status?`, `Data?`, `String?`, `[Fault]` (CONVERT by its fields' rows), an enum whose payload has static metadata (no metadata initialization) and nils alone, through native Optional and Array witnesses, equal native arm64 under JIT, `--interp` and AOT; refused without rows, with rows of another UUID, cdhash, offset, size, metadata initialization, static metadata offset or field, and with the static metadata linked into writable `__DATA` |
| guest/svfacts | PASS | 1 | the walk's per-type facts from a fixture (test switch `HAO_SWIFT_WALK_FACTS`, a TEST_SWITCHES build), tuples beside a `C??` nil through native tuple witnesses: a SAME row passes an `InlineArray` the rules refuse, a CONVERT row's masked case images convert the `C??` nil, a REFUSE row refuses an `Int?` the rules certify; `HAO_SWIFT_VALUE_AUDIT=1`: the refused value goes on, equal to native, with one `hao-swift-audit` record (table key, chain, caller image UUID and offset, no address or path); the release `build/hao` ignores the audit and says so once (run under `HAO_SWIFT_VALUE_REPORT=1`, it reports the value, which goes on); `HAO_SWIFT_KEYS=1`: the SAME row's type is one `hao-swift-key` record (answer `same`, where its metadata lies, no address), no key twice, and `--stats` counts it |
| guest/svguest | PASS | 1 | guest-defined types' rows by identity (test switch `HAO_SWIFT_GUEST_FACTS`, a TEST_SWITCHES build; the fixture written from the built binary's LC_UUID, descriptor offsets and both architectures' measured layouts and case images): runtime-laid multi-payload enums (SAME; CONVERT with a `Data?` payload) a struct of `Status?`, `Data?`, `String?`, `[Fault]` (CONVERT by its fields' rows), an enum whose payload has static metadata (no metadata initialization) and nils alone, through native Optional and Array witnesses, equal native arm64 under JIT, `--interp` and AOT; also equal without rows and with rows of another UUID, cdhash, offset, size or metadata initialization (the walk's runtime-laid enum rule); refused with a row of another field or static metadata offset, and with the static metadata linked into writable `__DATA` |
| guest/svmpenum | PASS | 1 | a runtime-laid-out guest multi-payload enum with three empty cases inside a guest struct with layout-string witnesses, its Optional and Array copied by native witnesses: certified by the tag byte, equal native arm64 under JIT, `--interp`, AOT and `HAO_SWIFT_VALUE_STRICT=1`, with no Swift value line. Before the rule it was refused (not listed for report-only) |
| guest/svnoncopy | PASS | 1 | noncopyable native types (`Mutex<Int>`, `Mutex<AsyncStream<@Sendable () -> ()>.Continuation>` under the generated table's key; value witness flag IsNonCopyable) in boxes guest code makes: SAME fixture rows (`HAO_SWIFT_WALK_FACTS`, a TEST_SWITCHES build) admits it, equal to native arm64 under JIT, `--interp` and AOT; without the rows each is refused; with them a `Mutex<Guest>` and a guest `Mutex<Int>` of a module named Synchronization are refused |
| guest/viewselfnil | PASS | 1 | guest code builds a SwiftUI view inline (an `AnyView??` nil in arm64's encoding) and a native View modifier copies it; hao lends self with native nils and converts the guest's later calls of that type's native witnesses (the Bluetooth Settings pane's `sheet`) |
| guest/stripview | PASS | 1 | a release-style (stripped) guest whose closures SwiftUI calls with the result buffer in rax: an alert's message closure in a stack slot, GeometryReader contents in self lent to a View modifier, in a bare body's result, in a modifier argument, behind a guest View's @ViewBuilder property and in a guest ViewModifier's body (Calculator's and Bluetooth Settings' alert) |
| guest/viewbuffer | PASS | 1 | a guest View body copies guest-defined enums with layout-string value witnesses (one boxed past the inline buffer, one inline, holding `Data?` and `String?`) through `Any` while SwiftUI evaluates it; equal native arm64 under JIT, `--interp` and AOT. The body's value scope opens only with the image's SwiftUI interface units: there it was refused before (`initializeBufferWithCopyOfBuffer`, then an enum's `initializeWithCopy`) |
| guest/layoutxi | PASS | 1 | guest structs with layout-string value witnesses, laid out at run time from a frozen `Data` beside resilient `UUID`/`Date` fields, get arm64's extra inhabitant count (12, not x86_64's 252), which their compiled tag witnesses select a field by; their Optionals go through `DispatchQueue.sync`, Combine `removeDuplicates`, Arrays and generic code. Needs the layout-string compiler flags, which the script passes |
| guest/sitepack | PASS | 1 | a native Swift initializer (library evolution, no interface unit) whose Apple arm64 stack arguments are packed at natural size (two Bools at [sp] and [sp+1], then two addresses); hao repacks them into SysV slots from the guest call site's stores. The values are printed by native code and copied through `swift_arrayInitWithCopy` (findmylocateagent's `Friend.init`) |
| guest/xpcrect | PASS | 1 | guest messages no method implements, to an NSXPC remote object proxy (anonymous listener, same process), an NSProtocolChecker and a guest forwarder: placed by the forwarded method's signature, so a struct of four floats, a CGRect and struct results arrive as natively (the Dock's context menu position) |
| guest/xpcreply | PASS | 1 | guest code calls NSXPC reply blocks of five types (anonymous listener, same process): native forwarding blocks that share CoreFoundation's `__forwarding_prep_b___` as invoke, each placed by its own signature. Typed by the first reply's `{CGSize=dd}` for all, the mixed reply lost arguments and the seven-string one crashed in `_replacedWithDistantObject` (gamed's NSXPC reply crash) |
| guest/sitepack | PASS | 1 | native Swift functions (no interface unit) whose Apple arm64 stack arguments are packed at natural size; hao repacks them into SysV slots from the guest call site's stores when the slots the x86_64 callee reads confirm the layout. Ten cases, each its own process: findmylocateagent's `Friend.init` shape (printed natively, copied through `swift_arrayInitWithCopy`), a `str q0` spill in the call sequence, mixed widths, and `encode` (`UserSubscriptionContextData.init`'s shape: four constant Int32 in one `stp xzr, xzr, [sp, #-32]!`, the values encoded by native `JSONEncoder` inside a guest Codable value) must equal native arm64; constant Bools merged into one `strh`/`str w` must be split by the callee's read widths, never shifted, and equal native. Also runs `stackpack.swift` without its unit; also a class method called through its dispatch thunk (the slots of the implementation its method descriptor names); runs with `HAO_SWIFT_VALUE_PASSTHROUGH=1` (its arrays hold an Optional of the helper's struct, which no frozen-struct table covers) |
| guest/asyncnil | PASS | 1 | native generic functions with a `G?` result (ThrowingTaskGroup/TaskGroup/AsyncStream `next()`, Dictionary and Set iterators at -Onone) leave G?'s nil in the x86_64 encoding in guest storage; converted before guest code reads it (Finder's `objc_retain(0x2)`); a child task with a `Node??` result is refused at its creation by default, equal to native with `HAO_SWIFT_VALUE_PASSTHROUGH=1` |
| guest/dynfunc | PASS | 1 | Swift `dynamic` functions (free, methods, accessors and `modify`, static, generic, throwing, subscript, async; an `@objc dynamic` Bool read by KVC and observed by KVO) pass their own address to the native `swift_getFunctionReplacement`; given a native entry point instead, each called itself forever (Music's window never appeared) |
| guest/oslog | PASS | 79 | `log show`; was a failure that depended on the checkout's path, see below. Under heavy parallel load one `oslogswift` mode can miss its records; it passes alone |
| guest/signals | PASS | 36 | |
| guest/sret-forward | PASS | 3 | |
| guest/stackargs | PASS | 2 | integer, float and float-HFA arguments on the arm64 stack, packed at their natural size, both to a native method and from native code to a guest block |
| guest/stackguard | PASS | 1 | |
| guest/svarchdiff | PASS | 1 | the walker on guest values holding Swift.Float16 (declared differently by architecture, frozen): refused; its Float16 refusals are checked only where this host's libswiftCore is the build `src/archdiff_gen.inc` describes; generic Float16 sizes, copies, Optionals and collections through the native metadata (a stub hao gives a 2-byte POD's witnesses) equal native arm64 under JIT, `--interp` and AOT |
| guest/mkgeometry | PASS | 2 | MapKit geometry functions passing or returning 32-byte HFAs (MKMapRect, MKCoordinateRegion) by value, including two without a header: native arm64 results under JIT, `--interp`, helpers-only and AOT; the six rows `src/aggspi_gen.inc` added for them fail without them |
| guest/optdata | PASS | 30 | Foundation.Data? arguments and result of a native Swift library through interface-unit kind D: native arm64 results under JIT, `--interp`, helpers-only and AOT; without the unit a nil argument arrives as an empty Data (the control) |
| guest/stackpackfp | PASS | 30 | a native Swift initializer whose packed arm64 stack arguments (CGFloat? tags, Bools) end with a CGFloat, typed by an interface-unit kind K record that lists the CGFloat as an 8-byte slot: native arm64 values under JIT, `--interp`, helpers-only and AOT; without the unit the call-site layout gives the same values |
| guest/swifttextalign | PASS | 30 | `NSTextAlignment` (Center and Right are 1 and 2 on arm64, 2 and 1 on x86_64) through a native Swift library: a property setter and getter, a parameter after a `String?` and an object, an unlabeled parameter and a result, converted by their demangled names; native arm64 results under JIT, `--interp`, helpers-only and AOT. Before the conversion the native side read a guest's center as right |
| guest/svconformance | PASS | 2 | published existentials: a guest conformance to a native protocol (class-bound and opaque) refused as not certified (not known wrong; a zeroed witness table word is), a guest protocol's existential with guest conformances admitted |
| guest/swift-object-callbacks | PASS | 5 | |
| guest/swiftcallee | PASS | 1 | guest closures passed to a native generic Swift function return T through its indirect result buffer; the native helper's symbol table is emptied so dladdr names nothing, and the callee is Swift by the guest's bound import name |
| guest/swiftlate | PASS | 4 | |
| guest/swiftls | PASS | 5 | |
| guest/swiftmeta | PASS | 8 | |
| guest/swiftpop | PASS | 4 | |
| guest/swiftthunks | PASS | 6 | |
| guest/threads | PASS | 2 | |
| guest/uresume | PASS | 26 | |
| guest/varsniff | PASS | 2 | |
| guest/xinhab | BLOCKED | — | local sandbox denies the Rosetta CPU query; regression requires the universal app helper to run as guest ARM code; every line matches native arm64 in JIT, interpreter and AOT; native-app-library override asserts the untyped-pointer mismatch |
| guest/xpac | PASS | 1 | |
| guest/ymmupper | PASS | 1 | upper ymm halves a native callee leaves dirty are clear after the call and on a native-to-guest callback, JIT and `--interp`, with and without ROSETTA_ADVERTISE_AVX=1 |
| guest/wx | PASS | 4 | no memory writable and executable at once, and no writable mapping of executable memory, after translated code, chained branches, thunks and Swift island entries have run (Rosetta's own memory aside); the program's `__DATA_CONST` read-only |
| guest/wxguard | PASS | 1 | guest mprotect, munmap, mmap MAP_FIXED, Mach VM calls and the raw mprotect on hao's executable memory are refused, and on `__DATA_CONST` with write (natively allowed) |
| guest/refuse | PASS | 3 | a call or callback whose signature cannot be placed stops with `bridge: REFUSED` (with HAO_NO_CLOSURES at registration); a callback's result without a buffer goes to hao's own area, not over the guest stack, and one larger than the area faults in its guard |
| guest/run-async | PASS | 9 | |
| guest/keypath-identity | PASS | 2 | |
| guest/late-method-signatures | PASS | 10 | |
| guest/native-bundle | PASS | 5 | loads a plug-in `.bundle` into a command-line guest |
| guest/objc-class-order | PASS | 6 | |
| guest/objcvascope | PASS | 2 | |
| guest/stubcat | PASS | 20 | an `@objc` extension on a Swift class with resilient ancestry (a category on its ObjC class stub, `__objc_catlist2`): responds, conforms, protocol dispatch, KVC and a class method equal native arm64 under JIT, `--interp` and AOT. Without attaching, the setter is an unrecognized selector (the control) |
| guest/unarchbool | PASS | 3 | NSUnarchiver decoding with arm64's `@encode(BOOL)` ("B") of archives holding 'c' and 'B': value, array, struct, multiple values, a struct named with a 'B', no size; native arm64 output under JIT, `--interp` and AOT. Without the conversion, an archived 'c' throws NSArchiverArchiveInconsistency (the control) |
| guest/valist | PASS | 10 | Swift's `__VaListBuilder` as guest code inlines it with the arm64 layout: `withVaList` into `vsnprintf`, a list whose storage grows, `String(format:)`, an empty list and 2,000 builders made and released equal native arm64 under JIT, `--interp` and AOT. Without `src/valist_builder.c` the first growth aborts on the unresolved `deallocStorage` (the control) |
| host/runtime | PASS | 2 | |
| host/chain | PASS | 6 | |
| host/commpage | PASS | 1 | |
| host/diagsig | PASS | 1 | diagnostics from a signal handler: kept lines, a fatal report opening a waiting log, the cap line; an inserted library counts formatting and allocation calls (none allowed). With the boot header formatted by `snprintf`, `fatal` fails (the control) |
| host/contract | PASS | 1 | `contract.json` through the vendored hao-patcher checker (`tools/hao-contract`, pinned in `PIN`): no tracked file names a home folder or a private name (names from the local private-names file; `tests/aot/policy.c` is allowed its example home folders), `generated.lock` matches every `src/*_gen.inc` byte for byte, cache tables carry `HAO_GEN_OS_BUILD` and every format is `HAO_GEN_FORMAT`; the vendored copy matches its pin. A hand-edited table fails |
| host/catalog-gate | PASS | 17 | `tools/hao_catalog_gate.py` on a synthetic cache: entries read from the pointer-keyed table and the flat list (one missing from either is still checked); a catalog Optional with libswiftCore's generic witnesses passes, one with a compiler-specialized witness fails closed; the fast-path count; generated.lock, value contracts (cache UUID, anchor bytes) and code changed under an unchanged UUID, unless only by reviewed edits (`--accept-edits`); a rebuilt family's images by LC_UUID, `__TEXT` size and export offsets |
| host/sigpolicy | SKIP (expected) | 1 | the profile cases need `PROFILE`; the hao cases x86_64 hardware |
| host/near-map | PASS | 1 | |
| host/dscselhoist | PASS | 1 | |
| host/swiftvalue | PASS | 1 | |
| host/svidentity | PASS | 1 | a SAME fixture row (TEST_SWITCHES libhao) applies to the runtime's own metadata of 19 native types (generic, conformances of a first and a second parameter, a generic class, noncopyable `Mutex`es, tuple, class, foreign, metatypes, function) and not to a heap copy of it with the same name and layout |
| host/swiftlayout | PASS | 4 | builds one Swift program for arm64 and x86_64, runs both natively (the arm64 one needs Apple silicon) and loads SwiftUI, Network and AppIntents in both, without making a view, to resolve some of their enum types; the x86_64 one also dlopens ComputationalGraph. Section N walks nils of standard types (pointer words, single inhabitants, tag bytes, Data translated); section M existential metatypes and byte-value carriers (Bool, payload-free enums, SwiftUI's `ToolbarItemGroup`); section F, with a TEST_SWITCHES libhao (`LIB=build-ts`), fixture rows of the facts table (source-aware pointer nils, masked CONVERT images, REFUSE before the small-POD rule), skipped with a release libhao. Its native-type table checks (section E) apply where this host's images are the build `src/mpenum_gen.inc` and `src/archdiff_gen.inc` describe; elsewhere it prints a note and skips those labels |
| objc | PASS | 5 | |
| tools | SKIP (expected) | 25 | runs bundle, stubdeps, platform, identity and aotpayload; stamp cases need `KC` (a kernel collection); MenuBarAgent case needs `HAO_FS` |
| tools/release | PASS | 1 | python3.14 |
| translate | PASS | 3 | |
| fuzz-smoke | PASS | 100 | 20 s per target, random seed |

Not entries of `tests/check.list`:

| Script | Why |
|---|---|
| `tests/aot/run.sh` | `make aot` (in `make test`) |
| `tests/guest/legacy-fixups/run.py`, `tests/guest/indexed-mulhigh/run.py`, `tests/host/async_package.py` | in `make test` |
| `tests/host/jitlock.sh` | `make jitlocktest` (in `make test`) |
| `tests/tools/{bundle,stubdeps,platform,identity,aotpayload}.sh` | run by `tests/tools/run.sh` |
| `tests/fuzz/run.sh`, `tests/fuzz/fuzz_hao_fat.py` | `make fuzz-smoke` |
| `tests/fuzz/make_seeds.sh`, `tests/guest/bridgeunit_gen.py`, `tests/aot/patch.py` | generators and helpers, not tests |
| `tests/guest/bench.c`, `tests/guest/vbench.c`, `tests/guest/vbench2.c` | timing programs for `hao --stats` and wall-time comparisons (`vbench`: single-precision vector arithmetic and LD1/ST1; `vbench2`: TBL/TBX, EXT, MUL/MLA/MLS, FRINT*, 64-bit compares, min/max, ABS), not tests |

`make unit` runs `tests/aot/jit_vector.c`: 552,518 cases of the
translated vector floating-point, FRINT* (vector and scalar), integer
compare/min/max/multiply, TBL/TBX/EXT, shift/narrow/widen and LD1/ST1/LD1R
encodings (with neighbouring encodings that stay with the helper), each
through the translator and the interpreter from one random state under one
of the 16 FPCR RMode/DN/FZ settings, alone and with NZCV live across it.
Registers, NZCV, the full FPSR (IEEE flags included), FPCR and memory must
be equal; a store that faults straddling into an inaccessible page may
differ only in the bytes before the page end that the interpreter's
`memcpy` wrote (see docs/ISSUES.md). FMLA/FMLS .2S/.4S, three-same and
by element, include directed near-ties, all four indices, high Vm registers,
register aliases, all 16 FPCR settings, full FPSR comparisons and maximum
96-instruction blocks (including the emitter's scratch capacity).
FP16 .4H/.8H adds all eight H:L:M indices with the restricted Vm bank,
normal/subnormal/overflow boundaries, a late inexact pair after earlier
admitted pairs, FZ16/AHP helper routing, and maximum-length blocks.
The unit build redirects the descriptor helper through a counting wrapper
and asserts that ordinary exact cases stay inline and a known inexact
binary64 sum declines. This checks admission even when its lost low bits do
not affect the final half result; production helper routing is unchanged.
`aot/fmla-mutations` first requires an unmutated control to pass, then builds
private copies with FP32 odd/negation/index logic or FP16 rounding increment,
subnormal shift, H:L:M decode, exactness admission or product negation broken,
or the FP64 exact product, IXC admission or operand window.
Each must produce a result/state or admission mismatch. The exactness
mutation is detected by helper routing, not by an assumed result difference. `guest/fma32` and `guest/jitinsns` compare
native arm64 with JIT, interpreter, helpers-only and AOT, including these
forms. `guest/fmla-bench` checks native result equality and reports ns/FMLA
for three-same and by-element .4S/.8H, JIT/AOT; timings are informational,
with no machine-dependent performance threshold.
`--stats` reports executed interpreter-helper calls as `helper_calls`,
separately from `helper` (translated helper sites). Dynamic counting is
enabled only with `--stats`; use separate runs for uninstrumented timings.

No script was left out for launching a GUI application, bundle, extension
or XPC service: `tests/tools/bundle.sh` only signs and inspects its `.app`
copies, `cli.sh` runs a bundle-shaped command-line tool by path, and
`native-bundle` loads a plug-in bundle with `CFBundle` in a command-line
process.

## Known failures

No tests are currently marked `xfail`.

`guest/xinhab` keeps the program and its universal Swift helper in the ARM
representation domain. With `HAO_NATIVE_APP_LIBS=1`, a second run in each
mode checks the known mismatches from passing Swift enums through untyped
pointers to the native x86_64 helper. Bridge-specific helper tests explicitly
set this switch to preserve their native-call coverage. `guest/dlrpath`
checks both policies for a universal library opened at run time.

Fixed: `guest/oslog` failed for the Swift program only (every `oslogswift`
record `<compose failure [UUID]>` under JIT, `--interp` and AOT), and only
in some build directories. No switch was involved. `tools/hao-embed` gave a
stub an LC_UUID made from the program's arm slice LC_UUID alone, and logd
saves the format strings of a UUID once (`/var/db/uuidtext`) and resolves
every later record of that UUID in the saved copy. `oslogswift` builds to
the same arm slice in every checkout, so all its stubs shared one UUID,
while the stub's `__cstring` moves with the length of the `--host-test`
rpath (the build directory): at offset 0x930 in the checkout whose stub
logd saved first, 0x950 in `build/` and `build-ts/` of a worktree with a
longer path. The C program never failed because its arm slice carries
`$OUT` in a load command, so each run had a new UUID. The stub's LC_UUID
now also names the stub's sections (addresses and bytes), as a linker's
UUID does; the script checks that its two different stubs of a program
have two UUIDs. The same defect reached installed systems: a program
embedded again with another libhao build kept its UUID.

Fixed: `guest/uresume`, case `alarmnx`, could exit 132 (SIGILL) when its
SIGALRM handler made an NX callback during the first callback's
Objective-C enum initialization. The captured RIP was
`_os_once_gate_recursive_abort+0x17`: the handler's callback reentered the
same unfinished `pthread_once`, not a failed signal return. The enum
bridge now defers guest signals around the complete `pthread_once` call.
[Measured scope and remaining limitations](../docs/ISSUES.md) record the
bounded before/after counts and native ARM comparison.

Fixed: `test_fork` (`forktest`, and `guest/fork`'s host stress) could die
with SIGBUS in JIT emission on the Rosetta host, more often under load. The
cause is Rosetta's, not hao's: a write to a page of x86 code Rosetta has run,
on one thread while another forks, kills the process. Under Rosetta the
parent's JIT workers now translate without running; `JIT_RUN=1` restores
the old behaviour. The earlier SIGSEGVs in bridge binding were the label
initialization race (see [remaining issues](../docs/ISSUES.md)).

Host load: `make oracle`'s `atomic16` race cases once failed their control
("two 8-byte halves (must tear)") with 0 torn loads of 300000 at a load
average of about 20, because the writer thread barely ran during the reads.
Each case now starts reading once its writer runs and repeats its loads
until the writer has stored at least as often (the control: until it tears),
for at most `ATOMIC16_SECONDS` (30). A case that cannot establish the race
prints SKIP and does not fail; `make oracle` does not go through
`tests/check.sh`, so such a SKIP shows only in its output (the last line
counts "races not established"). A torn atomic load always fails.

`guest/footprint` measures every process with `MallocMaxMagazines=4` and
judges the smallest of up to three runs of a case over budget. Without the
cap, the `objc` JIT case grew by 300-400 KB of Malloc Tiny/Small and page
tables under a load average of about 20 (libmalloc's per-CPU magazines on
this 16-CPU host) and failed its 2048 KB budget in two of three full gates;
the code and data categories did not move.

## Sanitizer check

`make oracle-asan` builds the interpreter, SIMD/FP code, x86 encoder,
`src/jit.c` and the oracle checker with `-fsanitize=address,undefined`
(objects in `build/asan/`), runs `ORACLE_ASAN_N` records per mode (default
20000, about 36 s) and fails on any report; the reports stay in
`build/asan/oracle-*.log`. It is part of `make check`.

It reports nothing now. AddressSanitizer never did. UndefinedBehaviorSanitizer
reported 22 locations in the SIMD interpreter (`src/a64_simd.c`) at
`ORACLE_ASAN_N=200000`; other seeds reached the same two shift helpers from
three more instruction groups. They were of two kinds:

- Shifts by 64, 128 or -64 in encodings AArch64 leaves unallocated because
  their wide elements would be 128 bits, which hao executed: size 3 of the
  three-different group (all but PMULL), of SHLL, SADDLP/UADDLP/SADALP/UADALP,
  XTN/SQXTUN/SQXTN/UQXTN and the integer reductions across lanes (also
  32-bit lanes without Q), SSHLL/USHLL with `immh = 1xxx`, and the copy
  group with `imm5 = x0000`. They are undefined instructions now, as on
  hardware; PMULL also rejects `U = 1` and the 16- and 32-bit sizes. The
  three-different group has no reserved encoding left that hao accepts, so
  its entry is gone from `known_lenient` in `tests/oracle/known.h`.
- Signed overflow on valid encodings whose result wraps: the widening
  add, subtract, absolute-difference, multiply and accumulate forms
  (three-different and by-element), the SDOT/UDOT accumulator, and a left
  shift of a negative value in SQSHL/UQSHL/SQSHLU by immediate. They use
  unsigned arithmetic now.

Measured after the change: `make oracle` (200000 records per mode) and
`make oracle-asan ORACLE_ASAN_N=200000` (twice, different seeds) match
native arm64 on every record with no sanitizer report.

`guest/state-bench` compares native arm64, JIT, interpreter, helpers-only
and AOT for table-driven BLR/RET calls (compact and 8-KiB-spaced targets),
BR tail dispatch, alternating return sites, a single return site, and
short arithmetic blocks surrounding an XPACI helper. It prints timing
without counters, then collects `--stats` separately. Default N=100000;
`N=100000000 ENGINES='jit aot'` makes longer measurement runs. The result
checksum must match native; timings have no pass/fail threshold.

`aot/round16` (also in `make unit`) compares the specialized finite
binary64-to-binary16 rounding with the unchanged general converter: all
31,744 finite positive half intervals, endpoints and adjacent double values,
midpoints and their adjacent doubles, both signs and all four rounding
modes, plus random finite binary64 values (2,577,664 cases). It checks full
FPSR with clear/preset sticky flags and that the converter itself leaves the
FP environment unchanged. This exhausts boundary classes, not all doubles
inside each interval. `guest/fma16` checks 42 vector fused forms, including
all indexed lanes and aliases, against native arm64 in JIT, interpreter,
helpers-only and AOT, under the 16 RMode/FZ/DN settings. FZ16/AHP behavior is
outside this native comparison: the existing interpreter does not model it.

The FP16 guest also faults after ninety inline operations in a 96-instruction
block and checks the recovered guest PC and live NZCV against native execution
in every engine. This exercises deterministic re-emission into the enlarged
256 KiB recovery buffer. The mutation manifest timeout is 600 seconds for
its control plus eight independent variants.

`guest/helper-stats` checks dynamic decoder-group statistics in JIT and AOT:
1000 each of untranslated RBIT, FABD and integer SQADD, plus
1000 subnormal FADD declines,
with 1000 admitted FADDs excluded; N=0 subtracts startup calls. Without
`--stats`, no group lines are printed.

`guest/coverage-bench` compares all 112 SIMD arrangements and indexed forms with native arm64 in
JIT, interpreter, helpers-only and AOT. Constant ordinary operands and sixteen
operations per loop make each instruction's helper cost measurable. Timing
runs omit statistics; separate runs report helper groups. Loop/reset overhead
is included. Development-host ratios are descriptive; Intel measurements decide
performance acceptance. `N` controls iterations (default 50000).

`aot/integer-mutations` builds private translator copies and verifies that
the complete-state vector test passes unchanged and catches reversed ZIP
half selection and an ADDV reduction at the wrong lane width.
`guest/jitinsns` covers all 42 allocated ZIP/UZP/TRN forms, the five ADDV
arrangements and all 24 signed/unsigned pairwise-long arrangements against
native arm64 in JIT, interpreter, helpers-only and AOT.

`aot/fp-coverage-mutations` runs a passing vector-test control and a private
translator with the FADDP pair ordering deliberately corrupted. The complete
state test must detect the mutation; source files and the built runtime remain
unchanged. `guest/jitinsns` also includes vector/scalar S/D FADDP and all four
single-precision min/max reductions, compared with native in every engine.

`aot/register-shift-mutations` runs the vector differential test first on an
unmodified translation, then with the right-shift count negation removed.
The control must pass and the mutation must report an architectural mismatch.
The vector test covers every signed low-byte count (-128..127) for USHL/SSHL
.2S/.4S/.2D, ignored upper count bits, aliases and live NZCV.

Vector division cases cover .2S/.4S, aliases, full FPSR, all 16 FPCR controls
and directed exact/subnormal results. The FP coverage mutation script also
suppresses FDIV result admission; a directed exact tiny result asserts the
required helper route even if its architectural bits happen to agree.

The vector differential and native instruction tests also cover indexed
scalar FMUL S/D and FMLA/FMLS S, every index, high source register numbers,
aliases, upper clearing, FP special values and directed fused rounding.
The full-state cases use all 16 RMode/FZ/DN settings and full FPSR;
ordinary inputs with inactive signaling NaNs must remain inline.

The vector unit checks all five FP32 comparisons with zero in .2S/.4S,
including signed zeros, aliases, full FPSR and all 16 FPCR settings, with
ordinary/tiny helper-route assertions. `jitinsns` now checks every FP form
under all 16 RMode/FZ/DN settings in each engine against native arm64.

The tail-coverage scalar tests add FMADD/FMSUB/FNMADD/FNMSUB S, FNMUL S,
FRECPS and FRSQRTS S/.2S/.4S, all FPCR rounding/FZ/DN settings, aliases,
fused near-ties, inactive lanes and full-state comparisons with live NZCV.
The FP coverage mutation script additionally corrupts the scalar fused
addend sign and omits the reciprocal-square-root step's exact factor of 1/2;
each must fail the complete-state test. The native instruction and ordinary
coverage benchmark include all eleven forms. AOT coverage prints four
decimal places to distinguish changes below one tenth of a percent.
The integer-tail cases in `aot/jit_vector` cover widening add/subtract/multiply,
accumulation, SHLL, saturating narrowing and QC, SQ(RD)MULH vector/by-element,
REV, every SRI/SLI shift and scalar ADDP, including aliases and live NZCV.
Scalar SIMD B/H memory checks include inaccessible pages and writeback.
`guest/jitinsns` adds 229 native comparisons (FPSR included), and
`guest/coverage-bench` adds 219 integer instruction loops. `aot/integer-mutations`
also suppresses QC and selects the wrong wide operand half; each independent
mutation must be caught after an unmodified control passes.

The conversion tail cases in `aot/jit_vector` cover explicit scalar-to-GPR
rounding, H/S/D SIMD integer conversions, scalar H/S/D format conversions, vector FCVTN/N2 and FCVTL/L2,
register aliases, unsupported and reserved encodings, all sixteen FPCR
settings, sticky FPSR, and helper routing for ordinary inputs. D-to-S
includes FZ input and output cases that previously lost IDC or added IXC.
`guest/jitinsns` compares those forms to native in all four engines;
`guest/coverage-bench` adds twenty-six ordinary conversion workloads.
`aot/fp-coverage-mutations` additionally replaces all conversion rounding
modes with truncation and requires the complete-state test to detect it.

`aot/structure-mutations` requires a passing unmodified vector test, then
checks that incorrect interleaving and publishing load destinations before
the final memory access each fail. The vector test covers all LD2/3/4 and
ST2/3/4 multiple-structure arrangements, immediate/register post-index,
wrapped register lists, SP, base/offset aliases, and every byte boundary in
a 64-byte transfer into an inaccessible page. `guest/jitinsns` checks all
126 arrangement/addressing combinations against native ARM in every engine;
its signal probes additionally check recovered PC, NZCV and writeback when
interleaved transfers fault partway, skip or resume after page repair.
`guest/coverage-bench` includes 42 interleaved memory forms for native
checksums, separate helper statistics and uninstrumented timings.

`aot/wide-fp-mutations` checks a passing control and then disables packed
double/sqrt result admission; complete-state and exact-tiny helper-route
checks must reject it. `jit_vector` covers FADD/FSUB/FMUL/FDIV .2D and
FSQRT .2S/.4S/.2D, every FPCR RMode/FZ/DN setting, aliases, zero signs,
NaNs and range boundaries. `jitinsns` compares all seven forms with native
ARM in each engine; `coverage-bench` adds per-form checksum/timing/statistics
cases with ordinary inputs.

Half arithmetic cases cover FMUL/FADD/FSUB .4H/.8H, every indexed FMUL lane,
scalar H arithmetic and scalar/vector FMIN/FMAX/FMINNM/FMAXNM. The tests use
all sixteen FPCR settings, aliases, special operands, signed zeros, exact
and inexact half rounding boundaries and ordinary-input helper assertions.
`guest/jitinsns` compares these with native across all engines, while
`guest/coverage-bench` adds 37 ordinary half forms. The FP coverage mutation
script changes half subtraction into addition; `MUTATIONS` can select a
subset for focused development runs, with the full set remaining the default.

The FP environment tail cases add FCCMP/FCCMPE S/D under every condition,
all input NZCV, ordinary and exceptional operands, and FPCR controls. FPCR/FPSR
moves include XZR, writable-mask checks, seeded x87 exceptions and comparison
of x87 rounding bits as well as full FPSR and live NZCV. `guest/jitinsns`
adds 66 native forms; `guest/coverage-bench` adds eight loops. The FP coverage
mutation script independently reverses FCCMP's condition and corrupts the
FPCR rounding mapping.

FPCR state cases additionally seed a pending unmasked x87 invalid exception
and rewrite unchanged FPCR in all four rounding modes, with NZCV live and
dead. The JIT must skip the waiting FLDCW as the interpreter does. Rosetta
preserves the exception flag but does not deliver this pending exception;
Intel execution remains necessary to validate fault delivery itself.

Tail coverage expands the full-state corpus and x87 checks. The vector
unit and each mutation executable have a 240-second bound. Mutation runs
set `HAO_VECTOR_FAIL_FAST`: after a witnessed state/admission mismatch they
exit unsuccessfully, while every passing control still runs the entire
corpus. This avoids spending the remainder of the corpus on a mutation
already proved detectable. Suite-level timeouts remain bounded.

`aot/estimate-mutations` checks a full passing vector-test control and detects
independently corrupted estimate-table rounding and exponent parity. The
complete-state cases include all table buckets, both fraction edges, aliases,
late-lane rollback and exceptional boundaries. `guest/jitinsns` compares all
S/D scalar/vector reciprocal estimates with native ARM over every table
index, exponent parity, sign and FPCR control; ten benchmark loops measure
the ordinary paths. AOT format 7 had an unchanged header layout and rejected
version 6 caches because emitted code can use the new runtime table pointer.

`aot/cpu-profile` translates under the profile without optional CPU
features (include/hao/cpufeat.h) and checks that all four AES instructions
(aliases included) and PMULL/PMULL2 .1Q use the helper with complete
architectural state, and that CRC32C never does. It compares translation
bytes and instruction boundaries across normal, AOT and fault-recovery
emission under profiles 0 and AES+PCLMULQDQ, checks that a block of an AOT
cache is re-emitted under the cache's profile, and that the generator
identity is the plain libhao UUID. It never executes an instruction of a
feature it only pretends to have. `aot/crypto-mutations` requires a passing
control and detects an omitted ARM AddRoundKey, PMULL2 reading the low
halves and CRC32CW consuming a halfword. The vector differential test covers
all four AES forms, PMULL in every size, Q and U, and CRC32/CRC32C with every
sf/size pair; `jitinsns` adds native comparisons for separate/aliased AES
sources in each engine, and the oracle's dp records include CRC32/CRC32C
(one in sixteen) and its simd records PMULL; `make oracle` checks simd again
with the JIT under profile 0. The policy test (`aot` F) covers the feature
decoding (Ivy Bridge, Rosetta, Rosetta advertising AVX, AVX-512 state) and
the subset rule with the missing features named; `aot` P1-P5 generate with
`--cpu=0` and `--cpu=baseline` and, on a TEST_SWITCHES build, reject a cache
under `HAO_CPU_FEATURES=0`. `aot/cpu-haswell` uses Rosetta's
`ROSETTA_ADVERTISE_AVX=1` for a Haswell-class profile: oracle simd/dp under
it, identical `--cpu=baseline` caches with and without it, and a cache
needing AVX2 that loads only with it. The installer test keeps only caches
of the installed libhao within the baseline profile; `tools/aotpayload`
checks baseline-profile payload creation, installation and cache loading.

The oracle's `known` mode sweeps 1,024 candidate encodings per remaining
`known_unimpl` and `known_lenient` entry, with native outcomes and seeded input
state. Every checker mode reports actual tolerated outcomes per entry and
engine; the full `known` sweep fails when an entry has zero hits in either
engine. Partial random-mode runs report counts without demanding coverage.
Already implemented instructions must be removed from the exception list.
`guest/halfops` compares half arithmetic, comparisons, conversions and indexed
forms, plus S/D FRECPX, with native results and full FPSR in all four engines.
`guest/extramisc` checks signed/unsigned saturating unary and accumulating
operations, bounded FP rounding and half reductions; `guest/i8mm` checks
mixed-sign dots and integer matrix multiplication. `guest/widenfp` checks
half and BF16 widening fused operations and BF16 narrowing; `guest/bfdot`
checks BF16 dots and matrix products, including preservation of exception
flags. All compare native arm64 with interpreter, JIT, helpers and AOT,
including aliases, exceptional inputs, rounding controls and sticky flags.

The native oracle now rejects any IEEE FPSR flag difference in every mode.
`guest/fpscalar` compares 158 forms under all sixteen RMode/FZ/DN settings;
legacy scalar arithmetic no longer skips FZ cases. Both S/D variants,
register/zero comparisons and the operand admission boundaries are included.
The integrated instruction implementation has no `known_unimpl` exceptions;
`known_lenient` continues to describe the separate reserved-encoding gaps.

`tests/guest/fpscalar_stable.sh` measures all twenty affected scalar S/D
forms on fixed normal inputs and a separate destination, so multiply/divide
do not converge to zero or infinity. It checks result equality with native
ARM and reports the minimum of seven interleaved timing runs; timings have
no acceptance threshold. `BEFORE=/path/to/older/hao` adds a before column,
`HAO` selects the current engine, and `N`/`REPEATS` control the sample size.
The dependent-operation loops remain available through `fpscalar -b`.

`host/labels-race` pauses an unlocked re-export reader after it captures the
name-table pointer, then runs a second binder. The control must finish; a
private library with the old initialization guard must crash. `N=200` runs
200 fresh processes per variant. This startup race does not require fork.

`tests/host/mapjit_fork.c` is a standalone platform diagnostic with no hao
linked. Build with `clang -arch x86_64 -O2 -pthread`, ad-hoc sign using
`tools/hao.entitlements`, and run under `timeout -s KILL 30`. Repeat with
`-arch arm64` for a native control. Each process performs 300 forks while
other parent threads emit and execute code. Rosetta makes a MAP_JIT page
`r-x` once it has run x86 code from it and makes it writable again on the
next write; during a fork that write instead ends the process (SIGBUS,
terminated by Rosetta's exception handler, so a SIGBUS handler does not
run). The rate depends on host load: 0 of 200 serial runs, 76 of 200 with
12 at a time. `mapjit_fork noexec` writes without running: 0 of 200, against
28 of 200 without it (interleaved, eight at a time). It is not a gate entry;
`test_fork` avoids it as described above.

`oracle` and `oracle-asan` include `reserved`: 30,016 selector combinations
in nine SIMD groups, compared with native ARM for acceptance, values and flags.
The `known` sample continues to exercise those groups after exception removal;
adding a lenient exception fails the checker build. The exhaustive mode fixes
GPR operands away from x18, so opcode/immediate fields equal to 18 are tested.
`guest/reserved-simd` checks nine rejected encodings through all four engines
(including loaded AOT caches), two valid ignored-bit aliases, and every unsigned
reciprocal/reciprocal-square-root estimate bucket at both ends. Undefined guest
instructions currently terminate with the runtime's diagnostic and SIGABRT;
the oracle checks the internal UNDEF result against hardware SIGILL.

`make wx-cow-test` signs `hao` with and without the copy-on-write defeature
entitlement and checks that only the entitled copy refuses JIT, with the same
output.
`make jit-arena-test` redirects hinted anonymous data allocations 8 GB from
JIT memory. It checks that the code cache survives, plain and signature
thunks address the correct tables, async records remain in relative reach,
and code/data protections stay separate at 1, 256 and 1024 MiB cache sizes.
It also checks fork inheritance/COW, failed initialization, real writable
code aliases, and Swift task groups/native async callers under redirection
against native ARM output. The fixture fails against 256dd30 with the original
ENOMEM diagnostic.
