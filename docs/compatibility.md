# Compatibility

hao targets the ARM64 and ARM64e code that macOS 27 ships, running on the
x86_64 libraries and kernel of an Intel Mac.

## Supported

**Instructions.** Base A64 integer, branch, load/store and system
instructions; LSE atomics, exclusive and acquire/release (including RCpc)
forms, with 16-byte atomicity through CMPXCHG16B; scalar and vector floating
point in half, single and double precision with FPCR rounding modes, FZ, DN
and FPSR exception flags; integer SIMD, including saturating, widening,
narrowing and dot-product forms; AES, SHA-1, SHA-256, SHA-512, EOR3 and PMULL.
arm64e pointer-authentication instructions are accepted: signing and
authentication leave pointers unchanged, and XPAC strips as the hardware does.

**Execution modes.** JIT, interpreter and AOT caches run the same instruction
semantics. Instructions without a native translation run through the
interpreter as a helper from translated code.

**Loading.** Chained fixups and legacy dyld info streams (rebase, bind, weak
and lazy binds), ARM-only dylibs and bundles loaded by guests or by native
`dlopen`/`NSBundle`, re-exports, interposing, thread-local variables and
initializers. C++ and Objective-C exceptions and `longjmp` unwind across guest
and native frames.

**Native libraries.** Guest code calls the native x86_64 frameworks directly.
The bridge converts arguments, results, variadic calls, structures and HFAs,
C++ standard library types with differing layouts, Objective-C messages and
blocks, and Swift calls described by mangled names, metadata and interface
units.

**Callbacks.** Native code calls guest blocks, function pointers, Objective-C
method implementations, Swift value witnesses, closures, protocol witnesses
and async continuations. Signals are delivered to guest handlers with ARM
register state.

**Swift.** Classes, structures, enums with supported layouts, generics,
existentials, key paths, actors, task groups and async/await across the
boundary, with runtime-sized values handled where the layout is proven.

## Known limitations

**Instructions**
- SVE, SME and SM3/SM4 are treated as undefined instructions.
- FPCR.AH, FIZ and NEP (FEAT_AFP) are RES0: hao does not advertise FEAT_AFP,
  so a write to them is ignored and they read back 0. Newer Apple silicon
  keeps them and changes some FP results.

**Execution**
- Only code in loaded guest images is translated. ARM64 code a program
  generates at run time (a JIT writing to `MAP_JIT` or heap memory) does not
  run: a call into memory the guest mapped or made executable with `mmap` or
  `mprotect`, or into memory that is not executable on the host, stops the
  program with a `bridge: REFUSED` diagnostic. Executable memory the guest
  creates by other means (`mach_vm_*`) is not recognised, and a call there
  runs as x86_64 code, as does a first call made from a guest signal handler
  or a native call through a pointer to such code.
- Raw `svc` system calls pass to the kernel, except: `sigaction`,
  `sigprocmask` and `__pthread_sigmask` take the same path as the libc calls
  (a trampoline in a raw `sigaction` is not called; hao runs the handler as
  libc's trampoline would), and `bsdthread_create`, `bsdthread_register` and
  `sigreturn` fail with `ENOTSUP` and a diagnostic.

**Loading**
- Lazy binds are resolved eagerly; `dyld_stub_binder` is not implemented.
- Threaded binds, text relocations and strong-definition coalescing are refused.
- Guest images are not unloaded after `dlclose`.

**Native boundary**
- Private APIs without a type description are bound by inference from the
  guest's code or left UNKNOWN; their values and ownership are not proven.
- Untyped C function exports of guest bundles are not callable from native code
  through `dlsym`.
- Some Swift value layouts (foreign nominal types, frozen or generic payload
  enums outside the proven cases) are refused rather than guessed.

**Identity**
- A routed program runs under hao's identity and without its entitlements;
  embedding preserves them ([security model](security-model.md)).

**Performance**
- Translated code is slower than native ARM code; sustained heavy workloads
  are not yet tuned.

## Apps with their own entitlements (Xcode and similar)

An arm64-only app that is not signed by Apple and carries entitlements does not
start through hao's launcher: LaunchServices reports "incorrect executable
format", and hao refuses with "claims entitlements hao cannot grant; embed it
with tools/hao-embed". Such an app is embedded once, so that each of its
programs starts under its own path, signing identifier and entitlements:

```sh
cp -R /Applications/Xcode.app ~/Xcode.app
python3 tools/hao-embed --scan ~/Xcode.app > programs.txt
python3 tools/hao-embed --in-place $(cat programs.txt)
```

- Each arm64-only program gets an x86_64 slice: hao's stub, signed ad hoc with
  the program's own signing identifier and entitlements. The program's arm64
  code is not changed; hao checks it against the record the stub carries.
- Embedding relinks the stub, so it needs Apple's linker for macOS 27 (the
  Command Line Tools or Xcode). On an Intel Mac that runs macOS 27 under hao,
  embed the app on a Mac that has them, with the same app version, and copy
  the embedded files over.
- The stubs' ad hoc signatures run on Macs set up with hao's boot
  configuration, as the macOS images hao is built into are.
- An app update replaces the embedded programs: embed it again.
- Laptop test 2026-11-14, image f112087 (26B131): Xcode 27.2 embedded this
  way starts and stays up for about 3.5 minutes, until a plug-in load fails.

## Testing

Every instruction and boundary change is compared with native ARM execution of
the same code on Apple silicon:

- The instruction oracle runs randomized instruction sequences natively and
  through hao and compares the complete register, flag and memory state.
- Guest tests build a native ARM64 reference and an ARM64e guest from the same
  source, run the guest under JIT, interpreter and AOT, and compare output.
  Native helpers built universal stand in for system libraries.
- Loader, interface-unit, code-signature and type-encoding parsers have unit
  tests under ASan and UBSan and fuzz targets ([tests/fuzz](../tests/fuzz/README.md)).
- Packaging tools are tested on generated fixtures (`tests/tools/`).

Applications, extensions and system services are tested on the target system,
first in a virtual machine and then on MacBookPro9,2 hardware. A passing test
suite establishes the tested behaviour only; it does not establish that every
application works.
