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

The payload must hold exactly the files its `payload-manifest.json` lists,
each with its sha256: a file it does not list (a LaunchDaemon added under
`root/`, for example), a symbolic link or special file, a missing file other
than an AOT cache (made after the first boot), a listed file whose sha256
differs, a set-ID or sticky file under `root/`, or two `root/` names that are
one file on a case-insensitive volume fails the install before the first
write. The installer then copies every payload file it will read into a
private directory used through a descriptor, checks the copy's sha256
against the manifest and reads only the copy (checked by inode), so a
payload that changes during the install changes nothing that is installed.
It installs only the listed `root/` files, root:wheel, 0755 or 0644, each a
new file renamed into place through directory descriptors, and opens
programs to embed and files to replace the same way, following no link. The manifest's
hashes give consistency, not authenticity: whoever can rewrite the payload
can rewrite its manifest too, so the image builder must take both from an
authenticated source.

A hao stub (an executable x86 slice, of any subtype, that names libhao in a
library command of any kind or as a string; the payload's own
`/usr/libexec/hao` with its `__TEXT,__hao_cli` marker is the launcher, the
marker anywhere else proves nothing) must name its file's ARM slice in
`__TEXT,__hao_slice`. The installer embeds no stub that does not, gives a
manifest program that still carries an older hao stub this payload's stub
when that yields the recorded result, and fails if any other file in the
System volume carries a stub without that record.

The build-time tools (`hao-image-payload`, `hao-embed`, `hao-trust-list`,
`stub_identity_gate.py`, `hao-runtime-on-mac`, `hao_release.py`) assume a
build host whose inputs (the System root, the x86_64 cache, the hao build)
and working directories no other user can write. They refuse what they can
see breaks that (a shared temporary directory or output directory, an ACL
granting write, a link where a file is written) but do not defend against
another local user changing their inputs while they run. `hao-install.py`
additionally defends against hostile payload contents: what it installs is
only what the payload's manifest lists and hashes, read from private copies.
Neither makes the manifest authentic: that is the image builder's input.

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
limit must reach the signature and nothing may follow it. The signature's
own bytes past the code limit are not all sealed by its hashes, so nothing
the loader or the program uses may reach into them: no segment but a
read-only, non-executable `__LINKEDIT` (exactly that 16-byte name; so no
fixup slot), no section of any segment or type, zero-fill included
(initializer offsets and pointers among them), and no linkedit
data a load command names (fixups, exports, function starts, symbol
tables). A slice where one does is invalid. So every byte the loader maps as code or reads is
hashed. This is necessary because hao maps guest
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
   passes `codesign --verify --strict -R='anchor apple'` on the build host
   (codesign checks a private copy of the bytes the cdhash was computed
   from, so a file changed meanwhile is not listed under another's check). hao
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
check and the load. How the slice is taken depends on whether the file can
change at all:

| File | How its slice is taken |
|---|---|
| On the volume the system booted from, mounted read-only (the System volume, sealed or booted from an APFS snapshot: `MNT_ROOTFS` and `MNT_RDONLY`), owned by root, not writable by its group or others, without an ACL | Mapped. Its pages are shared with every process that maps the file, as natively |
| Any other file, root-owned or not: the data volume (`/Applications`, `/Library`, `/usr/local`), a disk image, a removable or network volume | Read once into memory. The mach header, load commands, signature, hashed pages, linkedit data and segments all come from that copy; rewriting the file, or the storage under it, during or after the load changes nothing in the process |

Owner, mode and ACL say who may write a file through its volume, not who
controls the storage under it: a disk image's owner can change a root-owned
file in it without any write bit. So only the read-only System volume is
mapped.

A copied image shares no memory with its file or with other processes. It
costs its slice's size in anonymous memory, per process, for as long as it
is loaded: 7.2 MB more peak footprint for an 8.4 MB library and 66 MB for a
67 MB one, measured. The programs that loaded them ran in 0.04 s and 0.21 s,
against 0.04 s and 0.20 s with the files mapped. For root-owned programs on
the data volume, which were mapped before, measured under Rosetta: the
Command Line Tools' `clang --version` (124 MB slice) went from 51 MB to
142 MB peak footprint, in 1.00 s both ways (median of 5); `git --version`
(3.8 MB) from 7.4 MB to 10.2 MB, 0.06 s both ways. Guest libraries are
taken the same way as the main executable.

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
  refuses non-platform native libraries of another Team ID. If the kernel
  does not take the flag, the program is not run (status 126): it would
  otherwise load native libraries that it cannot load natively;
- checks every guest library with `F_CHECK_LV`, and where the kernel cannot
  hold the 16 KiB-page signature, applies the same rule itself: a platform
  file (trusted list, else `anchor apple`), or a file of the main executable's
  Team ID signed by that team;
- checks each guest library's signature and page hashes as for the main
  executable. A bad page of an executable segment ends the process with
  SIGKILL and a CODESIGNING "Invalid Page" reason, as the kernel does natively.

A signed image's pages are hashed once in its process, whichever of these
routes then decides about it.

Natively neither the hardened runtime nor the kernel lets a program's
environment load code into it or vouch for code. So for a main executable
whose verified signature requires library validation, hao ignores every
open-tier switch that adds code or trust, before anything they name is
read or loaded, and names each on standard error (`HAO_IF_DIR ignored: the
program requires library validation`):

| Switch | What it does in an open-tier process | For a program that requires library validation |
|---|---|---|
| `HAO_NO_LIBVAL=1` | turns library validation off | ignored; library validation is on |
| `HAO_CACHE=DIR` | reads AOT caches (native code) from DIR | ignored; the system cache directories only: the user's own is not read either (`-v` says so) |
| `HAO_GUEST_ROOT=DIR` | takes guest OS libraries and the arm64e shared cache from under DIR | ignored |
| `HAO_IF_DIR=DIR` | reads interface units and loads the `.thunks` libraries (native code, whose initializers run) from DIR | ignored; `/usr/lib/hao/if` |
| `HAO_ASYNC_CONTRACT_LIBRARY=FILE` | loads FILE as the native async provider | ignored; the packaged provider |
| `HAO_TRUST_LIST=FILE` | takes the cdhashes in FILE as Apple's | ignored; `/usr/lib/hao/trusted-cdhashes` only |
| `HAO_NATIVE_APP_LIBS=1` | loads the app's own libraries natively first, outside library validation | ignored; they load as guest code |

The CLI route decides this on the slice its signature check takes, the
embedded route on the slice the loader then builds the image from, so the
decision and the image come from the same bytes. The other open-tier
switches select hao's own engines, diagnostics or bring-up behaviour, not
code or trust. Guarded and set-ID processes ignore all of these anyway.

dyld loads the stub's own dependencies, and runs their initializers, before
hao can set `CS_REQUIRE_LV`, and the stub's `disable-library-validation`
admits any of them. `tools/hao-embed` therefore links a program's native
dependency into its stub only from protected storage: the x86_64 shared
cache, or a file on the sealed system volume (not behind one of its
firmlinks to the data volume) or in the OS or App cryptex, when every path
dyld would try first, every directory and link on the way, and every
library that file loads at launch are such files too: every path in dyld's
search order (each rpath expansion and the fallback paths), not only the
one expected to load. The stub names libhao and libhao_identity by their
/usr/lib/hao paths, so dyld searches no rpath for them.
Any other native dependency (a bundle's framework, a file under /Library) is
loaded by hao's binder with the program's image, after library validation is
on.

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
be trusted. An on/off switch is on when set to anything but an empty value
or `0`.

| Process | Switches honoured |
|---|---|
| Open: an ordinary user's process | all |
| Guarded: PID 1, any process with effective uid 0, a process dyld treats as restricted, one holding entitlements beyond hao's own | `HAO_NO_AOT`, `HAO_INTERP`, `HAO_NO_CHAIN`, `HAO_JIT_HELPERS_ONLY`, `HAO_VERBOSE`, `HAO_CRASH_TRACE`, `HAO_NX_STATS`, `HAO_BRIDGE_STATS`, `HAO_DIAG_TIME` |
| Set-ID: credentials changed at exec | none |

The guarded list holds only switches that run the same code through another
of hao's engines or print. Everything that chooses code or a path
(`HAO_CACHE`, `HAO_GUEST_ROOT`, `HAO_IF_DIR`, `HAO_TRUST_LIST`,
`HAO_ASYNC_CONTRACT_LIBRARY`, `HAO_DIAG_DIR`, the command-line options carried
in `HAO_FLAGS`), turns a check off (`HAO_NO_LIBVAL`, for a program that does
not require library validation; `--allow-unsigned`) or changes how native
calls, signals or memory behave is for open processes only. Root is guarded whatever started it: `sudo` and login sessions pass
variables from users who are not root. With `-v`, hao names each switch it
ignored.

Boot arguments reach every process of a boot, set-ID programs and PID 1
included, so hao reads a fixed list: `hao_crash=1`, `hao_nx_stats=1`,
`hao_crash_trace=1`, `hao_bridge_stats=1`, `hao_diag_time=1`, `hao_swift_value_survey=1`, `hao_no_aot=1`, `hao_no_chain=1`,
`hao_jit_helpers_only=1`, and in PID 1 `hao=FLAGS` (`v`, `V`, `i`, `s`) and
`hao_verbose=N`. No boot argument turns library validation or the signature
check off.

`HAO_NATIVE_APP_LIBS=1` is an OPEN-tier diagnostic switch, read once at
startup. It restores native-first loading of app libraries, including guest
`dlopen`, even when they contain an ARM slice. The default keeps such
libraries in the guest ARM representation domain; OS libraries retain their
existing native binding policy. Guarded and set-ID processes ignore this
switch, and no boot argument enables it.

A build with `HAO_TEST_SWITCHES` defined is for hao's own tests and bring-up,
never for a release image. It also reads the test switches
(`HAO_LIBVAL_USER`, which also keeps `HAO_TRUST_LIST` for a program that
requires library validation, `HAO_SIGRETURN_USER`, `HAO_AOT_LIMIT_KB`,
`HAO_BRIDGE_ABORT`, `HAO_NO_JIT_MEMORY`, `HAO_CODE_ALIAS`, `HAO_AOT_LIST_DIR`,
`HAO_AOT_FREE_MB`, `HAO_CRASH`, `HAO_CPU_FEATURES`) from an open
process's environment, and the bridging and signal bring-up knobs as boot
arguments. `HAO_DISTRUST_ENV=1|2` is read by every build: it can only lower a
process to the guarded or set-ID rules.

With `hao_crash=1`, root processes log to `/private/var/log/hao/console.log`
and other users to a directory of their own under it. The logs hold register
dumps and image addresses, so each is readable by its owner only (0600). A
log directory that a user other than its owner may write, or a log file of
another user, is not used. A process that cannot write a log (an App Sandbox
one) sends the lines to the unified log as private records, which `log show`
prints as `<private>` unless the Mac logs private data.
## AOT caches

An AOT cache (`<LC_UUID>.aot`, written by `hao --aot [--cache DIR] FILE...`) is x86_64 code that
hao copies into its code cache and runs in place of translating the guest. hao
uses one only if both of the following hold.

### Who could write it

| Directory | Read by | Must be owned by |
|---|---|---|
| `/usr/lib/hao/aot`, `/private/var/db/hao` | every process | root |
| `~/Library/Caches/hao` (home from the user database, never `$HOME`) | that user's open-tier processes, for a program that does not require library validation | root or the user |
| `HAO_CACHE=/abs/dir` | an open-tier process that sets it, for a program that does not require library validation | root or the user |
| `hao --aot --cache DIR` | nothing: `hao --aot` writes there; a process reads it only as one of the rows above | root or the user |

The file, its directory and every directory above must have that owner, no
group or other write bit (a sticky ancestor is allowed outside the system
directories), no ACL entry that grants a write-type right (an ACL that
cannot be read counts as granting one), and no symlink the user owns on the
way. The file is read once into memory, checked there, and
those bytes are installed.

`hao --aot` writes into the last directory of that list its process reads:
`/private/var/db/hao` as root, the user's own directory otherwise, or
`HAO_CACHE` in an open-tier process. `--cache DIR` names the directory on the
command line instead. The command line is the choice of whoever runs the
command, which the environment of a guarded process is not, so `--cache` is
honoured at every tier except set-ID; a root process, which ignores
`HAO_CACHE`, needs it to write anywhere but `/private/var/db/hao` of the
machine it runs on (`tools/hao-image-payload` and `tools/hao_release.py` pass
it). DIR is checked exactly as `HAO_CACHE`'s directory is: every element must
be owned by root or the invoking user (by root, for root) with no group or
other write access, and a relative DIR is taken from the working directory.

### What it was translated from

Ownership says who wrote a cache, not which code it stands for. The LC_UUID,
vmsize and load-command hash in its header only name a guest: a file that
copies a program's mach header and load commands and carries other
instructions has all three. So the header (format v8) also carries the
identity of the code it was translated from, and the loader accepts a cache
only for an image whose own code gives the same identity.

| Guest | Identity | When `hao --aot` writes it | At load |
|---|---|---|---|
| Signed, signature validates | the cdhash of the slice hao runs, with its hash type | after every page of the slice matched that CodeDirectory | cdhash of the signature in the mapped image |
| Signed, signature does not validate | none | never: no cache is written | |
| Unsigned | SHA-256 over the mach header, load commands and executable segments | into a user, `HAO_CACHE` or `--cache` directory outside the system directories only | the same hash over the mapped image |

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

`hao --aot` with no FILE runs itself once per program of `/usr/lib/hao/aot-list`
(written by `tools/hao-image-payload`), else per path of
`/usr/lib/hao/stub-identities`, each as `hao --aot FILE` above. A list that is
not a root-owned regular file writable only by root (mode and ACL), or whose
directories from / are not, is not used. `hao --aot --auto` marks a run done
only when every listed program is there and succeeded, and believes a marker
only under the same ownership rules. A program whose cache in a directory this process reads already passes the
loader's checks is skipped; `hao --aot --check` runs those checks and writes
nothing. Both first verify the program's signature and, for a program that
requires library validation, look in the system directories only, as a run
of it does; without `--cache`, `hao --aot` writes no cache for such a
program into a directory a run of it does not read.

System directories hold caches for signed code only. `hao --aot` writes none
for an unsigned guest there (as root without `--cache`, or with `--cache` or
`HAO_CACHE` naming a system directory), the loader takes none from there, and
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
slice is unsigned or does not verify. Before anything else of the guest's
or anything the environment selects is loaded, hao verifies the slice
(structure, CodeDirectory, every page hash) and requires its cdhash to be
the one the stub names; otherwise it exits with status 126. A copy of an
embedded program with another ARM slice in it, even one with a valid
signature of its own, does not run under the program's identity and
entitlements. A stub without the record (made before `tools/hao-embed`
wrote it) names no slice and runs none: hao exits with status 126, and
the program must be embedded again. Whether the process's main executable
is a stub is said by the front end the kernel started (`hao-stub` or
`hao`), never by the record's absence. `tools/stub_identity_gate.py` checks the record of every stub
of a payload. The check hashes the program's ARM slice once per launch.

A stub whose program uses ObjC re-executes itself once to turn off libobjc's
tagged-pointer obfuscation, unless its parent's exec already did. It
re-executes the path it was started through while that still names its own
file, else the kernel's path for its vnode, and never another file. A process
whose credentials changed at exec (set-user-ID or set-group-ID: `issetugid()`,
or a real ID that differs from the effective one) does not re-execute.

### Apple identity for listed programs

The kernel runs an embedded program's stub, which is signed ad hoc. With
AMFI's checks off, as on the MacBookPro9,2 image (`amfi_get_out_of_my_way=1`),
the kernel already reports a stub much as it reports the native program:
platform binary, the program's signing identifier, no Team ID. Measured on
26B131 with `csops`, SecCode and SecTask for `loginwindow` and `Dock`. Two
things differ: the cdhash is the stub's, and the signature has no Apple
anchor. So every check of a code requirement (`anchor apple`,
`certificate leaf[...]`, `cdhash`) fails against a stub. An example is authd's
gate on `FUSAutologinDataKey`, which requires `anchor apple` and the identifier
`com.apple.loginwindow`. Native programs that the image builder re-signs ad hoc
fail the same way.

Two lists, both in `/usr/lib/hao`, say which code stands for which Apple
program (`include/hao/idlist.h`):

- `stub-identities` is written by `tools/hao-image-payload` through
  `tools/hao-trust-list --stubs`. It holds each embedded program's stub cdhash
  next to the cdhash of its arm64 slice. A stub is listed only if its signed
  `__hao_slice` record names that slice and the slice's cdhash is in
  `trusted-cdhashes`, that is, it is Apple's.
- `native-identities` is written by the image builder. It maps a native
  program it re-signed ad hoc to the Apple-signed original it was made from.

A list is read only if root owns it and every directory above it, none of
them can be written by group or others, none is a symbolic link and none has
an extended ACL (or one that cannot be read). A row whose reference is
itself listed is dropped, so a reference is never followed further.

The image's x86_64 Security.framework calls `hao_codeid_decide`
(`src/code_identity.c`, `build/libhao_codeid.a`, no libhao) after its own
validity checks. The decision changes nothing but a requirement failure
(`errSecCSReqFailed`) of a listed code. For running code, the code must first
pass the check with no requirement, so the running code is its file and its
cdhash is the kernel's. The listed reference is then opened at its path and
architecture and must have exactly the listed cdhash. The caller's own
requirement is evaluated against that reference's Apple signature, and that
verdict is returned. A listed stub therefore satisfies exactly the requirements
its Apple program satisfies. Kernel flags, entitlements and TCC grants do not
change, and `/usr/libexec/hao` (the routed path) is never listed.
`tests/tools/codeid.sh` checks this against Apple's `true` and `sleep`.

What this trusts, and what stops an attacker:

- **A stub someone else made.** Anyone can sign an x86_64 slice ad hoc and
  name an Apple arm64 slice in its `__hao_slice`. Its cdhash is not in the
  list, so it gets nothing. The list holds only stubs the image builder made,
  on the sealed System volume. Their x86_64 code is `hao-stub` and libhao, and
  hao runs only the arm64 slice the stub names, which it verifies first.
- **Rewriting a listed file.** The System volume is sealed, and the kernel
  checks the running stub's pages against its CodeDirectory.
- **`HAO_*` switches.** A process whose own cdhash is listed runs in the
  guarded tier (`src/trust.c`). A user's `HAO_TRUST_LIST`, `HAO_FLAGS` or
  `HAO_IF_DIR` therefore cannot load foreign code into a process that then
  passes as Apple's.
- **`DYLD_*` variables.** With AMFI's checks off, dyld honours
  `DYLD_INSERT_LIBRARIES` in every process. Measured on 26B131: the library
  was inserted into the stub of `/usr/bin/sw_vers`, a platform, hardened and
  `restrict` program, for an ordinary user and for root. The same policy
  applies to Apple-signed native programs; that is inferred, not measured. So
  on such an image any local user can already run code in a process that passes
  `anchor apple` with an Apple identifier, for example an Apple-signed x86_64
  program from an earlier release with an inserted library. The lists do not
  widen that. Where AMFI's dyld policy is in force, dyld ignores `DYLD_*` for a
  listed stub, because hao-embed signs the stubs of platform, hardened and
  library-validated (library flag) programs with the hardened runtime, and
  never with allow-dyld-environment-variables when the program requires
  library validation, and the guarded tier covers hao's own switches.
- **A translator bug reachable from input** inside such a process is the same
  class of risk as a memory bug in the native Apple program.

Because any user can run any ARM code through it, **`/usr/libexec/hao` must
never be given privileges**: no TCC grants, configuration profiles, extra
entitlements, platform or trust-cache status, or set-ID bits. Its own
entitlements (`tools/hao.entitlements`: `allow-jit`,
`disable-library-validation`) take no effect without the hardened runtime.

## Executable memory

hao's executable memory (translated code, thunks, Swift island entries)
is held to what a native JIT on an Intel Mac offers: it is never
writable where it runs, no lasting mapping writes it, and a guest cannot
change its mapping or protection, nor make an image's sealed constant
data writable, through libc, libsystem or raw system calls. This defends
against an attacker who can corrupt memory but not run code. Code
already running in the process, guest or native, is out of scope, as it
is for a native JIT; known and not defended against:

- the memory entries hao keeps for its writes can be mapped again by
  code that finds them and calls Mach;
- another thread can store into a write mapping while it is open (a
  burst of translated blocks, a chain patch, a thunk page), at a random
  page of a reserved window;
- VM requests sent as Mach messages to the task port are not seen by
  hao's checks of calls and traps.

On the Rosetta development host the executable mapping is briefly made
writable after each write so Rosetta drops stale translations; an Intel
Mac does not do that, and lowers the mapping's maximum protection to
read/execute.

## Known limitations

- A guest file on the read-only System volume is mapped, not copied. That
  volume is taken to be unchanging while it is mounted: sealed, or an APFS
  snapshot (an image whose seal was broken boots from one). A root process
  that remounts the root file system writable and rewrites such a file
  while hao loads it is not noticed; natively the kernel would kill the
  process at the changed page.
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
  An embedded program's stub keeps its own cdhash and ad hoc signature in the
  kernel. Only Security.framework's requirement checks answer for a listed stub
  as its Apple program (Apple identity for listed programs), and only where
  the image's Security.framework calls `hao_codeid_decide`. Checks that
  compare cdhashes through `csops` (a pinned cdhash, a trust cache) still see
  the stub. `SecCodeCopySigningInformation` still describes the stub.
- In the command-line route, native library initializers run before hao's
  check. If one of them asks for the main executable's path or sections, it
  is answered from the file `HAO_EXEC` names, unchecked. Its header and
  load commands are read once into memory (at most 4 MiB of them), and a
  segment whose carried sections span more than 64 MiB is not answered
  for. A file that fails the check never runs.
- An embedded program's stub carries `disable-library-validation`, and dyld
  loads the native libraries the stub links, and runs their initializers,
  before hao sets `CS_REQUIRE_LV`; `tools/hao-embed` links them only from
  protected storage (above). Library validation holds for what the program
  loads from then on.
- A stub made before `tools/hao-embed` wrote the `__hao_slice` record names
  no ARM slice and runs none (status 126). `hao-install.py` restubs such a
  program when the payload embeds it and otherwise fails the install while
  one is left in the System volume; a copy kept elsewhere (a user's folder)
  is not found and does not start until it is embedded again.
- A stub seals its program's launch and library constraints (signature
  slots 8-11) byte for byte; `tools/hao-embed` refuses a program with a
  constraint codesign cannot express (a category constraint) unless
  `releases/dropped-constraints.txt` pins that blob of that build (path,
  slot, ARM cdhash, blob sha256), and then the stub has none for that slot;
  the identity gate fails a stub missing any constraint not pinned there. A self constraint that asks for a platform binary cannot be
  met by an ad hoc stub, so such a program does not start where the kernel
  enforces launch constraints. Launch constraints that come from a trust
  cache rather than the signature do not apply to a stub.
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
  load, unless the program requires library validation. Such a program
  (most notarized applications are hardened) is translated afresh at each
  launch unless root put a cache for it in a system directory.
