# Security model

hao runs ARM code that the Intel kernel cannot validate itself. This page
describes how a guest reaches hao, which checks hao applies in place of the
kernel's, and what a guest keeps or loses of its identity.

## Exec routing

HaoExec registers an image activator that looks only at the first page of a
file: an `MH_EXECUTE` arm64 header, or a fat header with an arm64 slice and no
x86_64 slice. For such a file it hands the kernel the interpreter line
`#! /usr/libexec/hao --`, so a file whose name looks like an option is still
the program. The kernel validates only `/usr/libexec/hao`; the guest's
signature is checked by hao.

HaoExec is stamped for one kernel by `kext/stamp_identity.py`. The stamp holds
the unslid addresses of a few kernel functions; at load, HaoExec requires all
of them to give the same slide before it reads the kernel header, so a kext
stamped for another kernel never dereferences a guessed address. The activator
table must be exactly `[mach, fat, shell, NULL]`. Stamp every boot kernel
collection with the stamper from the same commit as the kext.

The interpreter path is a constant compiled into HaoExec. Nothing at run time
can change it: there is no `kern.hao.interpreter`, because a writable path
would let one sysctl write send every ARM exec on the machine to another
program. Root can turn routing off and on with `kern.hao.enabled`, and
`kern.hao.routed` counts routed execs.

Once its activator is installed, HaoExec refuses to unload. The kernel calls
the activator through a table slot without holding a reference on the kext,
so there is no moment at which its code is known to be unused. Set
`kern.hao.enabled=0` to stop routing; restart to remove the kext. HaoExec
writes the table slot once, at load, with one aligned pointer store, and
stays inactive on a kernel that has CR4.CET set.

Anyone can run `/usr/libexec/hao FILE` directly, so routing adds no privilege
and every check lives in hao.

## Installation

`tools/hao-install.py` runs as root on a System volume and takes file names
from the payload's manifests. Each name must be a relative path, already
normalised, without `..`: one that is not fails the install before anything
is written. The installer follows no symbolic link for such a name or for a
runtime file: a link in the payload's tree, among the directories of a
target, or where a file is to be written fails the install.
`tools/hao_release.py` applies the same rule when it checks and materialises
a bundle.

The packaging tools read Mach-O files, code signatures and interface units
from the image being built. Their readers share one bounded load-command walk
(`hao_fat.load_commands`) and refuse malformed or truncated input with an
error instead of hanging or failing with a traceback; `tests/fuzz` holds the
inputs that once did.

## Guest signature checks

Before running a guest through the command-line route (`hao PROGRAM`, which is
also every routed exec), hao checks the slice it will run (arm64e, else arm64):

| Guest | Result |
|---|---|
| Signature present but invalid (malformed, a page or sealed blob that does not match its hash, bytes outside the signed range, a CMS that does not sign this CodeDirectory) | refused |
| Unsigned | refused; `--allow-unsigned` (`HAO_FLAGS=u`) runs it, in an open-tier process only |
| Ad hoc or non-Apple signature, entitlements within hao's own (`allow-jit`, `disable-library-validation`, `get-task-allow`) | runs |
| Ad hoc or non-Apple signature claiming any other entitlement | refused: it would run without them, under hao's identity |
| Apple (listed, a platform binary to the kernel, or `anchor apple`) | runs; its entitlements are not carried |
| Set-user-ID or set-group-ID whose owner or group this process is not | refused |

Refusals exit with status 126 and a `hao[prog:pid]:` diagnostic; `-v` prints
the verdict, who gave it and the cdhash. A switch that the process's tier
ignores (`--allow-unsigned`, `HAO_FLAGS` or `HAO_TRUST_LIST` outside an
open-tier process) is named on standard error whatever the verbosity:
`hao: --allow-unsigned ignored (guarded process)`.

### Integrity

hao validates the embedded signature as xnu does: the superblob structure, the
CodeDirectory of highest hash rank and its cdhash, every page hash up to the
code limit, and the special slots of every blob the superblob carries
(requirements, entitlements, DER entitlements, launch constraints). The code
limit must reach the signature and nothing may follow it, so every byte the
loader maps or reads is hashed. This is necessary because hao maps guest
`__TEXT` as data, and the kernel enforces page hashes only for executable
mappings. The Info.plist, resource-directory and application slots name files
outside the Mach-O; exec does not check them, and neither does hao.

A slice has one signature and hao has one reader for it
(`hao_codesig_parse` in `src/libval.c`). A slice with a second
`LC_CODE_SIGNATURE` is not loaded, and a signature with two CodeDirectories
of one hash type is invalid. Integrity, the checks of this section, the
signer and the library-validation policy are all decided on the one
CodeDirectory that reader chose, in the bytes the image is built from.
`tools/hao_codesig.py` reads signatures for the packaging tools by the same
rules; `tests/tools/test_codesig.py` requires the same verdict, cdhash and
policy from both for 8168 inputs: the fuzz corpus and fixed changes to it.

### Who signed it

The first route that answers decides:

1. **Ad hoc**: nobody. Integrity is the whole check.
2. **Trusted cdhash list**: Apple. `tools/hao-image-payload` writes
   `/usr/lib/hao/trusted-cdhashes` with `tools/hao-trust-list`: the cdhash of
   every arm64-only program, library and bundle under the image's root that
   passes `codesign --verify --strict -R='anchor apple'` on the build host. hao
   reads the list only if root owns it and every directory above it, and
   nobody else can write them. `HAO_TRUST_LIST=/abs/file` adds a list in an
   open-tier process, for tests. A process reads the lists once, when it
   first needs them, and keeps what it read.
3. **Kernel**: `fcntl(F_ADDFILESIGS_INFO)` registers the slice's signature
   (AMFI validates the CMS with CoreTrust) and returns the cdhash the kernel
   chose, which must be hao's; `F_GETSIGSINFO` says whether it is a platform
   binary. A different cdhash means the file was rewritten in place and is
   refused.
4. **Security.framework**: the CMS over the CodeDirectory, then
   `anchor apple`. This is the only route that needs `trustd`, and only
   CMS-signed guests that are in no list and that the kernel does not register
   reach it. For the main executable hao asks in a short-lived fork of the
   process that is about to run the guest and takes only that child's exit
   status, for the cdhash the parent computed itself. The fork is this code by
   construction; no program is started by path to give the answer.
   Security.framework is loaded in the child only.

The list exists because an x86_64 kernel accepts only CodeDirectories with
4 KiB pages, and Apple's arm64 code is signed with 16 KiB pages: route 3
cannot answer for Apple arm64 code on Intel. It also means the check needs no
daemon, which matters for programs that launchd starts before `trustd`.

### Check and load the same bytes

The launcher runs in two stages. The first only re-executes hao with the
guest's arguments and the ObjC tagged-pointer switch. The second, which is
the guest's process, opens the file, takes the slice it will run once,
applies the checks above to those bytes and builds the image from them.
Security.framework, where it is asked, opens the path itself; its answer
counts only for the cdhash hao computed on the bytes it took.

Natively the kernel validates each page of a signed image as it pages it
in. Nothing does that for pages hao maps as data, so hao must not read a
guest file twice: whoever may write the file could change it between the
check and the load. How the slice is taken depends on who can change the
file:

| File | How its slice is taken |
|---|---|
| Someone other than root may change it: not owned by root, or writable by its group or by others, or with an ACL, or on a volume that is not local | Read once into memory. The mach header, load commands, signature, hashed pages, linkedit data and segments all come from that copy; rewriting the file during or after the load changes nothing in the process |
| Only root can change it (the system's own files; every file of a sealed system volume) | Mapped. Its pages are shared with every process that maps the file, as natively |

A copied image shares no memory with its file or with other processes. It
costs its slice's size in anonymous memory, per process, for as long as it
is loaded: 7.2 MB more peak footprint for an 8.4 MB library and 66 MB for a
67 MB one, measured. The programs that loaded them ran in 0.04 s and 0.21 s,
against 0.04 s and 0.20 s with the files mapped. Guest libraries are taken
the same way as the main executable.

The loader also bounds what a guest file may claim: 4 MiB of load commands,
256 MiB for an exports trie, chained fixups, a legacy dyld stream or
function starts, 4 MiB for a lazy-load record. The first segment must be
readable and hold the mach header and every load command, and a fat header
must name the cpusubtype its slice's own header has.

What the first stage passes in the environment is a hint and carries no
trust: `HAO_EXEC` is the path, `HAO_EXEC_FLAGS` the command line's switches.
Anyone can start hao as its own second stage; the file is checked all the
same. Each command-line switch is honoured where the environment switch of
the same effect is: `-v` as `HAO_VERBOSE` and `--interp` as `HAO_INTERP`,
`--stats` and `--allow-unsigned` in an open-tier process only. An inherited
`HAO_FLAGS` is honoured in an open-tier process only.

## Library validation

hao's processes carry `disable-library-validation` because `libhao` is ad
hoc signed. The policy is read from the main executable's ARM slice, from
the CodeDirectory whose signature and page hashes hao has just verified on
the bytes it loads; a main executable whose signature does not verify is not
run, in the embedded form too. When that slice is a platform,
hardened-runtime or library-flag binary without its own
`disable-library-validation`, hao:

- sets `CS_REQUIRE_LV` on the process once `libhao` is mapped, so the kernel
  refuses non-platform native libraries of another Team ID;
- checks every guest library with `F_CHECK_LV`, and where the kernel cannot
  hold the 16 KiB-page signature, applies the same rule itself: a platform
  file (trusted list, else `anchor apple`), or a file of the main executable's
  Team ID signed by that team;
- checks each guest library's signature and page hashes as for the main
  executable. A bad page of an executable segment ends the process with
  SIGKILL and a CODESIGNING "Invalid Page" reason, as the kernel does natively.

A signed image's pages are hashed once in its process, whichever of these
routes then decides about it.

A non-weak dependency that does not load ends the process as dyld does
(`dyld[PID]: Library not loaded: ...`, then `abort_with_payload` with
`DYLD_EXIT_REASON_DYLIB_MISSING`). In a test build (`HAO_TEST_SWITCHES`),
`HAO_LIBVAL_USER=1` takes the user-space route on any kernel, for the main
executable's signer too.

Where a guest library's signer has to come from Security.framework (for
example a CMS-signed library that is in no list, on a kernel that cannot hold
its signature), hao asks in the guest's process, which keeps the framework
loaded from then on.

In a set-ID process hao searches for a guest's dependencies as dyld does for
one: `@executable_path` is not expanded, `@loader_path` is not expanded in an
`LC_RPATH` of the main executable, and relative install names and relative
`LC_RPATH` entries are not tried. The caller of a set-ID program chooses its
working directory and, with a hard link, the directory its path names.

`tools/hao-embed` signs a platform or hardened program's stub with the
hardened runtime, so dyld ignores `DYLD_*` variables for the stub as it does
for the program.

## Environment and boot arguments

An embedded program runs with its own credentials and entitlements, and its
environment comes from whoever started it. hao reads its `HAO_*` switches
once, at startup, and honours them by how far the process's environment can
be trusted:

| Process | Switches honoured |
|---|---|
| Open: an ordinary user's process | all |
| Guarded: PID 1, any process with effective uid 0, a process dyld treats as restricted, one holding entitlements beyond hao's own | `HAO_NO_AOT`, `HAO_INTERP`, `HAO_NO_CHAIN`, `HAO_JIT_HELPERS_ONLY`, `HAO_VERBOSE`, `HAO_CRASH_TRACE`, `HAO_NX_STATS`, `HAO_BRIDGE_STATS` |
| Set-ID: credentials changed at exec | none |

The guarded list holds only switches that run the same code through another
of hao's engines or print. Everything that chooses code or a path
(`HAO_CACHE`, `HAO_GUEST_ROOT`, `HAO_IF_DIR`, `HAO_TRUST_LIST`,
`HAO_ASYNC_CONTRACT_LIBRARY`, `HAO_DIAG_DIR`, the command-line options carried
in `HAO_FLAGS`), turns a check off (`HAO_NO_LIBVAL`, `--allow-unsigned`) or
changes how native calls, signals or memory behave is for open processes
only. Root is guarded whatever started it: `sudo` and login sessions pass
variables from users who are not root. With `-v`, hao names each switch it
ignored.

Boot arguments reach every process of a boot, set-ID programs and PID 1
included, so hao reads a fixed list: `hao_crash=1`, `hao_nx_stats=1`,
`hao_crash_trace=1`, `hao_bridge_stats=1`, `hao_no_aot=1`, `hao_no_chain=1`,
`hao_jit_helpers_only=1`, and in PID 1 `hao=FLAGS` (`v`, `V`, `i`, `s`) and
`hao_verbose=N`. No boot argument turns library validation or the signature
check off.

A build with `HAO_TEST_SWITCHES` defined is for hao's own tests and bring-up,
never for a release image. It also reads the test switches
(`HAO_LIBVAL_USER`, `HAO_SIGRETURN_USER`, `HAO_AOT_LIMIT_KB`,
`HAO_BRIDGE_ABORT`, `HAO_NO_JIT_MEMORY`, `HAO_CODE_ALIAS`) from an open
process's environment, and the bridging and signal bring-up knobs as boot
arguments. `HAO_DISTRUST_ENV=1|2` is read by every build: it can only lower a
process to the guarded or set-ID rules.

With `hao_crash=1`, root processes log to `/private/var/log/hao/console.log`
and other users to a directory of their own under it. The logs hold register
dumps and image addresses, so each is readable by its owner only (0600).
## AOT caches

An AOT cache (`<LC_UUID>.aot`, written by `hao --aot FILE`) is x86_64 code that
hao copies into its code cache and runs in place of translating the guest. hao
uses one only if both of the following hold.

### Who could write it

| Directory | Read by | Must be owned by |
|---|---|---|
| `/usr/lib/hao/aot`, `/private/var/db/hao` | every process | root |
| `~/Library/Caches/hao` (home from the user database, never `$HOME`) | that user's open-tier processes | root or the user |
| `HAO_CACHE=/abs/dir` | an open-tier process that sets it | root or the user |

The file, its directory and every directory above must have that owner, no
group or other write bit (a sticky ancestor is allowed outside the system
directories), no ACL entry that grants a write-type right, and no symlink the
user owns on the way. The file is read once into memory, checked there, and
those bytes are installed.

### What it was translated from

Ownership says who wrote a cache, not which code it stands for. The LC_UUID,
vmsize and load-command hash in its header only name a guest: a file that
copies a program's mach header and load commands and carries other
instructions has all three. So the header (format v6) also carries the
identity of the code it was translated from, and the loader accepts a cache
only for an image whose own code gives the same identity.

| Guest | Identity | When `hao --aot` writes it | At load |
|---|---|---|---|
| Signed, signature validates | the cdhash of the slice hao runs, with its hash type | after every page of the slice matched that CodeDirectory | cdhash of the signature in the mapped image |
| Signed, signature does not validate | none | never: no cache is written | |
| Unsigned | SHA-256 over the mach header, load commands and executable segments | into a user or `HAO_CACHE` directory only | the same hash over the mapped image |

A cdhash covers the header, the load commands and the code, because the
CodeDirectory holds a hash of every page of the slice and hao requires the
hashed range to reach the signature. An executable segment that reaches into
the signature itself, whose bytes are not all sealed, gets no cache.

`hao --aot FILE` works on a private copy: it copies FILE into the cache
directory (created exclusively, mode 0600, unlinked once mapped), maps the
copy, checks the copy's signature and translates the mapping. The owner of
FILE can rewrite it during generation without effect on what is checked or
translated. Who signed the guest and what it is entitled to are not asked:
those decide whether hao runs a program, not what its code is, and only an
image with that cdhash ever loads the cache.

System directories hold caches for signed code only. `hao --aot` writes none
for an unsigned guest there (as root without `HAO_CACHE`, or with `HAO_CACHE`
naming a system directory), the loader takes none from there, and
`tools/hao-install.py` removes any from `/usr/lib/hao/aot`. Guarded processes,
which search only the system directories, therefore never hash guest code at
load.

A user's own directory needs no such rule. The user can write any file there,
with any header, and only that user's open-tier processes read it: the
ownership rule is the boundary. The identity still applies there, so that
`hao --aot` on a file the user does not trust cannot produce a cache that
another of the user's programs loads.

Cost at load, measured for x86_64 code under Rosetta 2 on an M4 Max: the
identity of a signed image takes 2 to 18 microseconds (`awk`, `zsh`, `vim`),
which does not show in a cache load of 0.7 to 24 ms (medians before and after
differ by less than 2% either way); an unsigned copy of `zsh` (544 KB of
code) loads 1.6 ms slower than before the identity existed.
`tests/aot/loadtime.c` measures a load.

## Identity

A routed process is hao's to the kernel: TCC, the sandbox and launchd see
`/usr/libexec/hao`, and every routed program shares that identity. What stays
hao's: `p_comm`, `proc_pidpath`, the `KERN_PROCARGS2` exec path, the code
signature and entitlements, the audit token, dyld's main image and the unified
log's process image. What is the guest's: the path native code asks for,
`proc_name`, and the main executable's sections, served from the guest file by
`libhao_identity` (`src/process_identity.c`).

Programs that need their own identity, entitlements or set-ID bits are
embedded with `tools/hao-embed`. Script interpreters must be embedded too: the
kernel does not interpret twice, so a script whose interpreter is a routed
program fails with `EBADARCH`.

### Embedded programs

An embedded program's x86_64 slice is the stub, which runs the ARM slice of
the same file. The stub loads that slice only from the file the kernel is
running: it asks the kernel for the device and inode of the vnode behind its
own Mach-O header (`proc_pidinfo`, or the `executable_file=` pair the kernel
passes at exec where a sandbox denies that), opens the executable's path,
and refuses with status 126 unless the descriptor is that file. The slice is
taken from that descriptor. A link in the path that was repointed after the
exec therefore cannot put another file's ARM code into the process.

The stub's signature does not cover the ARM slice, so the stub names the
slice it was made for. `tools/hao-embed` writes the hash type and cdhash of
the ARM slice's CodeDirectory into the stub's `__TEXT,__hao_slice`, which the
stub's own signature does cover, and refuses to embed a program whose ARM
slice is unsigned or does not verify. Before it loads the slice, hao verifies
it (structure, CodeDirectory, every page hash) and requires its cdhash to be
the one the stub names; otherwise it exits with status 126. A copy of an
embedded program with another ARM slice in it, even one with a valid
signature of its own, does not run under the program's identity and
entitlements. `tools/stub_identity_gate.py` checks the record of every stub
of a payload. The check hashes the program's ARM slice once per launch.

A stub whose program uses ObjC re-executes itself once to turn off libobjc's
tagged-pointer obfuscation, unless its parent's exec already did. It
re-executes the path it was started through while that still names its own
file, else the kernel's path for its vnode, and never another file. A process
whose credentials changed at exec (set-user-ID or set-group-ID: `issetugid()`,
or a real ID that differs from the effective one) does not re-execute.

Because any user can run any ARM code through it, **`/usr/libexec/hao` must
never be given privileges**: no TCC grants, configuration profiles, extra
entitlements, platform or trust-cache status, or set-ID bits. Its own
entitlements (`tools/hao.entitlements`: `allow-jit`,
`disable-library-validation`) take no effect without the hardened runtime.

## Known limitations

- A guest file that only root can change is mapped, not copied. If root
  rewrites such a file in place while hao loads it, hao does not notice;
  natively the kernel would kill the process at the changed page.
- Whether only root can change a file is judged from its owner, mode and ACL
  and from its volume being local. A local volume whose storage someone else
  can write (a disk image or removable disk mounted with ownership enabled)
  can hold root-owned files that change all the same; hao maps them.
- HaoExec hands hao a path, not the file the kernel examined, and hao opens
  it again. Routing grants nothing, so a path that names another file by then
  only runs that file under the same checks.
- HaoExec routes unsigned and set-ID files; hao refuses them afterwards.
- HaoExec clears CR0.WP on one CPU, with interrupts off, for the one store
  that installs its activator. A non-maskable interrupt taken in that window
  runs without kernel write protection.
- The installer checks for symbolic links as it walks a path. It assumes
  nothing else changes the System volume during the install, as in an image
  builder.
- A routed process cannot take on the guest's code identity or entitlements.
- In the command-line route, native library initializers run before hao's
  check. If one of them asks for the main executable's path or sections, it
  is answered from the file `HAO_EXEC` names, unchecked. A file that fails
  the check never runs.
- A stub made before `tools/hao-embed` wrote the `__hao_slice` record does
  not name its ARM slice. hao verifies such a program's ARM slice by the
  slice's own signature only, so a copy with another validly signed slice
  still runs under the stub's identity and entitlements. Embed such
  programs again; `hao-install.py` does not replace a stub that is already
  in a file.
- A set-ID embedded program that uses ObjC runs with libobjc's tagged-pointer
  obfuscation on: the stub does not re-execute there, and where `issetugid()`
  is set libobjc would ignore the switch anyway. Translated code that tests
  for tagged pointers inline can misread them.
  Programs that do not use ObjC (stubs marked `__hao_noreexec`) are not
  affected.
- A process that may not fork (a sandbox that denies `process-fork`) cannot
  ask Security.framework about a main executable: a guest whose signer only
  that route can give is refused there.
- The stub's own-file check, its check of the ARM slice it names, the fork
  that asks Security.framework and the loading of a guest file from a copy
  were tested under Rosetta on an Apple-silicon host. None has been run on
  an Intel kernel yet.
- The AOT loader takes a signed image's cdhash from the signature the image
  maps and does not hash the image's pages again; it relies on the loader's
  own check of that image.
- An AOT cache in a user's own directory is trusted as far as that user: any
  process of the user can write one that the user's other open-tier programs
  load.
