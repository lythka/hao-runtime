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

Anyone can run `/usr/libexec/hao FILE` directly, so routing adds no privilege
and every check lives in hao.

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
the verdict, who gave it and the cdhash.

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

### Who signed it

The first route that answers decides:

1. **Ad hoc**: nobody. Integrity is the whole check.
2. **Trusted cdhash list**: Apple. `tools/hao-image-payload` writes
   `/usr/lib/hao/trusted-cdhashes` with `tools/hao-trust-list`: the cdhash of
   every arm64-only program, library and bundle under the image's root that
   passes `codesign --verify --strict -R='anchor apple'` on the build host. hao
   reads the list only if root owns it and every directory above it, and
   nobody else can write them. `HAO_TRUST_LIST=/abs/file` adds a list in an
   open-tier process, for tests.
3. **Kernel**: `fcntl(F_ADDFILESIGS_INFO)` registers the slice's signature
   (AMFI validates the CMS with CoreTrust) and returns the cdhash the kernel
   chose, which must be hao's; `F_GETSIGSINFO` says whether it is a platform
   binary. A different cdhash means the file was rewritten in place and is
   refused.
4. **Security.framework**: the CMS over the CodeDirectory, then
   `anchor apple`. This is the only route that needs `trustd`, and only
   CMS-signed guests that are in no list and that the kernel does not register
   reach it. Security.framework is loaded only for this route, so it never
   stays in a guest's process.

The list exists because an x86_64 kernel accepts only CodeDirectories with
4 KiB pages, and Apple's arm64 code is signed with 16 KiB pages: route 3
cannot answer for Apple arm64 code on Intel. It also means the check needs no
daemon, which matters for programs that launchd starts before `trustd`.

### Check and load the same file

The check runs in the launcher's first stage, which then re-executes into the
guest's process. The first stage passes the checked file's device and inode to
the second (`HAO_EXEC_ID`); the second refuses to run without them and refuses
a mapped file that is not the one checked.

## Library validation

hao's processes carry `disable-library-validation` because `libhao` is ad
hoc signed. To give the guest the policy its own signature asks for, when the
main executable's ARM slice is a platform, hardened-runtime or library-flag
binary without its own `disable-library-validation`, hao:

- sets `CS_REQUIRE_LV` on the process once `libhao` is mapped, so the kernel
  refuses non-platform native libraries of another Team ID;
- checks every guest library with `F_CHECK_LV`, and where the kernel cannot
  hold the 16 KiB-page signature, applies the same rule itself: a platform
  file (trusted list, else `anchor apple`), or a file of the main executable's
  Team ID signed by that team;
- checks each guest library's signature and page hashes as for the main
  executable. A bad page of an executable segment ends the process with
  SIGKILL and a CODESIGNING "Invalid Page" reason, as the kernel does natively.

A non-weak dependency that does not load ends the process as dyld does
(`dyld[PID]: Library not loaded: ...`, then `abort_with_payload` with
`DYLD_EXIT_REASON_DYLIB_MISSING`). `HAO_LIBVAL_USER=1` takes the user-space
route on any kernel, for tests.

`tools/hao-embed` signs a platform or hardened program's stub with the
hardened runtime, so dyld ignores `DYLD_*` variables for the stub as it does
for the program.

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

Because any user can run any ARM code through it, **`/usr/libexec/hao` must
never be given privileges**: no TCC grants, configuration profiles, extra
entitlements, platform or trust-cache status, or set-ID bits. Its own
entitlements (`tools/hao.entitlements`: `allow-jit`,
`disable-library-validation`) take no effect without the hardened runtime.

## Known limitations

- hao reopens a routed guest by path. The device and inode check catches a
  swapped file, not one rewritten in place by its owner between the check and
  the mapping, or before hao reads a page it has not translated yet.
- HaoExec routes unsigned and set-ID files; hao refuses them afterwards.
- A routed process cannot take on the guest's code identity or entitlements.
- An embedded stub's signature does not cover the ARM slice. On a sealed
  system volume this does not matter; for embedded programs on writable
  volumes, hao does not yet check the ARM slice.
