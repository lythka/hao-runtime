#!/usr/bin/env python3
"""stub_identity_gate: the embedded stub's load-time identity against its arm64e slice, by rule.

The stub (tools/hao-embed) is the x86_64 slice the kernel runs for an
arm64-only program, so it is the main executable dyld, the kernel, AMFI,
the sandbox, LaunchServices and the frameworks' linked-on-or-after checks
look at. It carries every load command and header field of the program's
arm slice that they read, except a reasoned deny-list of the ones that
describe arm64 code. RULES is that table, one row per load command type:

  copy    the arm slice's command, byte for byte
  derive  the stub's own command, with the fields the OS reads taken from the
          arm slice (or computed from it by a stated rule)
  deny    not carried: it describes the arm64 code or link edit, which the
          stub does not contain (each with who reads it and why that is not
          the program's identity)
  refuse  hao-embed refuses the program (the stub cannot reproduce it)

A load command type the table does not name is refused by hao-embed and a
DIFF here: a new type must be decided, not carried or dropped by default.

  stub_identity_gate.py [--x86-cache DIR] [--host-test] [--out DIR] FAT...
  stub_identity_gate.py [--x86-cache DIR] --stubs DIR --root ROOT [--out DIR]
      FAT: an embedded file (its arm slice and its x86_64 stub); --stubs: a
      hao-embed --stubs payload, each manifest entry's stub against ROOT/path.
      --x86-cache: also check the linked dylibs against hao-embed's
      native_deps (the dependencies that load natively, as src/runtime.c
      binds them); without it they are checked for order, kind and version.
      --host-test: stubs made with hao-embed --host-test (public
      entitlements only, the build directory's rpath).

For each program and field: SAME, DIFF (a rule is not met: exit 1), KNOWN
(a denied field that differs, with the reason) or DENY (a denied command,
not compared). OUT/diff.tsv has every non-SAME row, OUT/programs.tsv one row
per program, OUT/summary.md the counts by field and verdict. Exit 2 on bad
input.
"""
import argparse
import collections
import importlib.machinery
import importlib.util
import json
import os
import plistlib
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hao_codesig  # noqa: E402
import hao_fat  # noqa: E402

# --- the rule table -------------------------------------------------------------------------------------------------
# cmd: (name, action, reason). Readers: dyld (dyld4 reads the main executable's platform, rpaths, environment,
# segments and __RESTRICT), the kernel at exec (LC_MAIN, __PAGEZERO, LC_UNIXTHREAD, segment protections, MH flags,
# the code signature), AMFI and the sandbox (signature, entitlements, CS flags), LaunchServices and CFBundle
# (__info_plist, bound Info.plist), linked-on-or-after checks (dyld_get_program_sdk_version: LC_BUILD_VERSION),
# libmacho users on the main header (getsectbyname and friends: src/process_identity.c answers what the stub
# does not carry from the guest's own header).
RULES = {
    0x32: ("LC_BUILD_VERSION", "copy",
           "dyld takes the process's platform (which images may load) from the main executable, and "
           "dyld_get_program_sdk_version/minos (the frameworks' linked-on-or-after checks) from its build "
           "version; copied whole, with the tool versions"),
    0x24: ("LC_VERSION_MIN_MACOSX", "derive",
           "the older form of the build version: platform macOS, its minos and sdk go into the stub's "
           "LC_BUILD_VERSION (dyld reads either)"),
    0x25: ("LC_VERSION_MIN_IPHONEOS", "refuse", "not a macOS executable"),
    0x2F: ("LC_VERSION_MIN_TVOS", "refuse", "not a macOS executable"),
    0x30: ("LC_VERSION_MIN_WATCHOS", "refuse", "not a macOS executable"),
    0x2A: ("LC_SOURCE_VERSION", "copy",
           "the program's source version: crash reports, spindump, vmmap and dyld_info show the main "
           "executable's"),
    0x8000001C: ("LC_RPATH", "copy",
                 "dyld expands @rpath for the main executable's dependencies and for every dlopen in a load "
                 "chain that reaches it (native libraries dlopen @rpath names); copied in the program's order "
                 "after the stub's own (/usr/lib/hao first, so no program directory can supply libhao into an "
                 "entitled process)"),
    0x27: ("LC_DYLD_ENVIRONMENT", "copy",
           "dyld applies the main executable's DYLD_*_PATH settings at launch (unless the process is "
           "restricted, which the stub is exactly when the program is)"),
    0xE: ("LC_LOAD_DYLINKER", "copy", "the kernel loads this dyld; must be /usr/lib/dyld in both"),
    0x80000028: ("LC_MAIN", "derive",
                 "the kernel sizes the main thread's stack from stacksize (copied); entryoff is the stub's "
                 "own code"),
    0x19: ("LC_SEGMENT_64", "derive",
           "the stub's own segments for its code and link edit; __PAGEZERO's size (the kernel reserves it) "
           "is the program's; every section native code looks for by name in the main executable's header "
           "is carried with its bytes and its segment's protections (hao-embed CARRY: __TEXT "
           "__info_plist/__launchd*/__xpcproxy/__config, every section of a segment the toolchain does not "
           "make: __RESTRICT (dyld: restricted process), __CGPreLoginApp, __SLSERVER, __CGSERVER, "
           "__INFO_FILTER, ...); the arm64 code and data sections are not (libmacho lookups of those are "
           "answered from the guest's header by src/process_identity.c)"),
    0x1B: ("LC_UUID", "derive",
           "a version 5 UUID of the arm slice's LC_UUID (hao-embed STUB_UUID_NAMESPACE): logd, crash "
           "reports and symbolication key on it, and the stub's code is not the program's"),
    0x1D: ("LC_CODE_SIGNATURE", "derive",
           "AMFI, the sandbox, TCC and SecCode read the signature: identifier, entitlements (plus allow-jit "
           "and disable-library-validation), the hardened runtime when dyld would ignore DYLD_* natively, "
           "the CS flags kill/hard/enforcement, special slots -1 (Info.plist) and -3 (resource seal); the "
           "signature is ad hoc over the stub's own pages"),
    0xC: ("LC_LOAD_DYLIB", "derive",
          "the stub links the program's dependencies that load natively, in order, with their versions "
          "(the images dyld has initialised when libSystem starts: libsecinit's sandbox); guest images, "
          "missing and lazy ones are not linked (hao-embed native_deps)"),
    0x80000018: ("LC_LOAD_WEAK_DYLIB", "derive", "as LC_LOAD_DYLIB, weak kept weak"),
    0x8000001F: ("LC_REEXPORT_DYLIB", "derive", "as LC_LOAD_DYLIB (a main executable re-exports to nobody)"),
    0x80000023: ("LC_LOAD_UPWARD_DYLIB", "derive", "as LC_LOAD_DYLIB"),
    0x20: ("LC_LAZY_LOAD_DYLIB", "derive", "not loaded at launch natively either: not linked (hao loads it on "
                                            "first call)"),
    0x2: ("LC_SYMTAB", "deny", "the arm64 symbol table; the stub has its own"),
    0xB: ("LC_DYSYMTAB", "deny", "the arm64 dynamic symbol table; the stub has its own"),
    0x26: ("LC_FUNCTION_STARTS", "deny", "arm64 function addresses (hao reads them from the arm slice)"),
    0x29: ("LC_DATA_IN_CODE", "deny", "data ranges inside arm64 code"),
    0x3A: ("LC_ATOM_INFO", "deny", "the linker's atom records for the arm64 code; nothing reads them at load"),
    0x80000033: ("LC_DYLD_EXPORTS_TRIE", "deny",
                 "the arm64 executable's exports name arm64 code, which native code cannot call; guest images "
                 "bind to them through hao's loader (src/runtime.c). A native dlsym(RTLD_MAIN_ONLY) of a "
                 "program-exported symbol finds the stub's trie, which has none (a known limitation)"),
    0x80000034: ("LC_DYLD_CHAINED_FIXUPS", "deny", "the arm64 slice's fixups; hao applies them to the guest image"),
    0x22: ("LC_DYLD_INFO", "deny", "the arm64 slice's binding opcodes; hao applies them to the guest image"),
    0x80000022: ("LC_DYLD_INFO_ONLY", "deny", "the arm64 slice's binding opcodes; hao applies them to the guest "
                                             "image"),
    0x2B: ("LC_DYLIB_CODE_SIGN_DRS", "deny", "the linker's record of the dylibs' designated requirements; "
                                              "the stub's signature is its own"),
    0x2E: ("LC_LINKER_OPTIMIZATION_HINT", "deny", "arm64 instruction hints for the linker"),
    0x2D: ("LC_LINKER_OPTION", "deny", "a linker input (object files' autolink options); not read at load"),
    0x31: ("LC_NOTE", "deny", "data for debuggers and core-file readers; not read at load"),
    0x2C: ("LC_ENCRYPTION_INFO_64", "deny",
           "cryptid 0: no encrypted range (the kernel decrypts only a nonzero cryptid's pages); a nonzero "
           "cryptid is refused: hao cannot read the program's encrypted code"),
    0x21: ("LC_ENCRYPTION_INFO", "deny", "as LC_ENCRYPTION_INFO_64"),
    0x4: ("LC_THREAD", "refuse", "an arm64 thread state the kernel would start"),
    0x5: ("LC_UNIXTHREAD", "refuse", "an arm64 entry state the kernel would start (hao-embed needs LC_MAIN)"),
    0x1: ("LC_SEGMENT", "refuse", "a 32-bit segment"),
    0xD: ("LC_ID_DYLIB", "refuse", "an executable that is also a dylib: the stub cannot be the dylib"),
    0x80000035: ("LC_FILESET_ENTRY", "refuse", "a kernel collection"),
}
COPY = {c for c, (_, a, _) in RULES.items() if a == "copy"}
# Header fields. cputype/cpusubtype: the stub is x86_64. filetype: MH_EXECUTE in both (hao-embed embeds no other).
# MH flags the kernel reads at exec are copied: MH_PIE (ASLR; an arm64 executable is always PIE),
# MH_ALLOW_STACK_EXECUTION, MH_NO_HEAP_EXECUTION. The others describe the link edit (NOUNDEFS, DYLDLINK, TWOLEVEL,
# WEAK_DEFINES, BINDS_TO_WEAK, HAS_TLV_DESCRIPTORS, NLIST_OUTOFSYNC_WITH_DYLDINFO, APP_EXTENSION_SAFE: a linker
# check) and are the stub's own.
MH_PIE, MH_ALLOW_STACK_EXECUTION, MH_NO_HEAP_EXECUTION = 0x200000, 0x20000, 0x1000000
MH_POLICY_FLAGS = MH_PIE | MH_ALLOW_STACK_EXECUTION | MH_NO_HEAP_EXECUTION
# CodeDirectory flags: copied: kill, hard, enforcement (the kernel's treatment of invalid pages); derived: runtime
# (hao-embed hardened()); denied: library-validation (src/libval.c sets CS_REQUIRE_LV on the process after
# libhao, which is not a platform library, has loaded), adhoc and linker-signed (how it is signed).
CS_HARD, CS_KILL, CS_ENFORCEMENT, CS_RUNTIME, CS_REQUIRE_LV = 0x100, 0x200, 0x1000, 0x10000, 0x2000
CS_COPIED_FLAGS = {CS_HARD: "hard", CS_KILL: "kill", CS_ENFORCEMENT: "enforcement"}
DENIED_FIELDS = {
    "sig.team": "an ad hoc signature has no Team ID",
    "sig.platform": "an ad hoc signature is not a platform binary: Apple's anchor and platform identifier "
                    "come only from Apple's signature or a trust cache (expected effect: TCC prompts for "
                    "anchor-apple requirements)",
    "sig.library": "library-validation: src/libval.c sets CS_REQUIRE_LV on the process once libhao is loaded "
                   "(signing it would refuse libhao, which is not a platform library)",
}
LC_NAMES = {c: n for c, (n, _, _) in RULES.items()}


def rule(cmd):
    """(name, action, reason) of load command type CMD, or None."""
    return RULES.get(cmd)


def name_of(cmd):
    return LC_NAMES.get(cmd, "0x%x" % cmd)


def check_rules(mo, what="arm slice"):
    """RuntimeError if the arm slice MO has a load command the stub cannot
    reproduce: a type RULES does not name, a refused one, or an encrypted
    range."""
    for cmd, off, size in commands(mo, what):
        r = RULES.get(cmd)
        if r is None:
            raise RuntimeError(f"{what}: load command 0x{cmd:x} has no stub identity rule "
                               "(tools/stub_identity_gate.py RULES)")
        if r[1] == "refuse":
            raise RuntimeError(f"{what}: {r[0]}: {r[2]}")
        if cmd in (0x2C, 0x21) and size >= 20 and struct.unpack_from("<I", mo, off + 16)[0]:
            raise RuntimeError(f"{what}: {r[0]} cryptid nonzero: the program's code is encrypted")
    flags = struct.unpack_from("<I", mo, 24)[0]
    if not flags & MH_PIE:
        raise RuntimeError(f"{what}: not MH_PIE (the stub is position independent)")


# --- Mach-O reading -------------------------------------------------------------------------------------------------
def commands(mo, what="Mach-O"):
    """(cmd, offset, cmdsize) of each load command of a thin 64-bit Mach-O (ValueError if malformed)."""
    if len(mo) < 32 or struct.unpack_from("<I", mo)[0] != 0xFEEDFACF:
        raise ValueError(f"{what}: not a thin 64-bit Mach-O")
    yield from hao_fat.load_commands(mo, what)


def cmd_bytes(mo, kind):
    return [bytes(mo[o:o + s]) for c, o, s in commands(mo) if c == kind]


def lc_str(b, at=8):
    o = struct.unpack_from("<I", b, at)[0]
    return b[o:].split(b"\0")[0].decode(errors="replace") if 8 <= o < len(b) else ""


def ver(v):
    x, y, z = v >> 16, (v >> 8) & 0xFF, v & 0xFF
    return f"{x}.{y}.{z}" if z else f"{x}.{y}"


def build_version_text(b):
    if not b:
        return "-"
    platform, minos, sdk, ntools = struct.unpack_from("<IIII", b, 8)
    tools = ",".join("%d:%s" % (t, ver(v)) for t, v in
                     (struct.unpack_from("<II", b, 24 + 8 * k) for k in range(min(ntools, (len(b) - 24) // 8))))
    return f"platform {platform} minos {ver(minos)} sdk {ver(sdk)} tools {tools or '-'}"


def source_version_text(b):
    if not b:
        return "-"
    v = struct.unpack_from("<Q", b, 8)[0]
    parts = [v >> 40, (v >> 30) & 0x3FF, (v >> 20) & 0x3FF, (v >> 10) & 0x3FF, v & 0x3FF]
    while len(parts) > 2 and parts[-1] == 0:
        parts.pop()
    return ".".join(map(str, parts))


def segments(mo):
    """{segname: (vmsize, maxprot, initprot, {sectname: (offset, size, flags)})}."""
    out = {}
    for c, off, size in commands(mo):
        if c != 0x19:
            continue
        seg = mo[off + 8:off + 24].split(b"\0")[0].decode(errors="replace")
        vmsize = struct.unpack_from("<Q", mo, off + 32)[0]
        maxprot, initprot, nsect = struct.unpack_from("<III", mo, off + 56)
        sects = {}
        for k in range(min(nsect, (size - 72) // 80)):
            so = off + 72 + 80 * k
            name = mo[so:so + 16].split(b"\0")[0].decode(errors="replace")
            ssize = struct.unpack_from("<Q", mo, so + 40)[0]
            foff = struct.unpack_from("<I", mo, so + 48)[0]
            sects[name] = (foff, ssize, struct.unpack_from("<I", mo, so + 64)[0])
        out[seg] = (vmsize, maxprot, initprot, sects)
    return out


def cd_fields(mo):
    """(identifier, flags) of the primary CodeDirectory, or (None, 0)."""
    for c, off, size in commands(mo):
        if c == 0x1D:
            base, length = struct.unpack_from("<II", mo, off + 8)
            if base + length > len(mo) or length < 12:
                return None, 0
            _, _, count = struct.unpack_from(">III", mo, base)
            for k in range(count):
                typ, boff = struct.unpack_from(">II", mo, base + 12 + 8 * k)
                if typ == 0:
                    cd = base + boff
                    flags, _, ident = struct.unpack_from(">III", mo, cd + 12)
                    return mo[cd + ident:cd + ident + 256].split(b"\0")[0].decode(errors="replace"), flags
    return None, 0


def load_embed():
    """tools/hao-embed as a module (its file name has a dash)."""
    path = os.path.join(HERE, "hao-embed")
    ld = importlib.machinery.SourceFileLoader("hao_embed", path)
    mod = importlib.util.module_from_spec(importlib.util.spec_from_loader("hao_embed", ld))
    ld.exec_module(mod)
    return mod


# --- the comparison -------------------------------------------------------------------------------------------------
def compare(arm, stub, embed, src_path=None, x86=None, host_test=False, stub_own_rpaths=None):
    """[(field, verdict, arm value, stub value, note)] for the arm slice ARM and its stub STUB (bytes).
    EMBED: the hao-embed module; SRC_PATH: the program's path (for dependency resolution with X86)."""
    rows = []

    def add(field, ok, a, s, note="", known=None):
        if ok:
            rows.append((field, "SAME", a, s, note))
        elif known:
            rows.append((field, "KNOWN", a, s, known))
        else:
            rows.append((field, "DIFF", a, s, note))

    # every load command type has a rule
    for c in sorted({c for c, _, _ in commands(arm)}):
        r = RULES.get(c)
        if r is None:
            add("lc." + name_of(c), False, "present", "-", "no rule in RULES")
        elif r[1] == "refuse":
            add("lc." + r[0], False, "present", "-", "refused: " + r[2])
        elif r[1] == "deny":
            rows.append(("lc." + r[0], "DENY", "present", "-", r[2]))
    # header
    af, sf = struct.unpack_from("<I", arm, 24)[0], struct.unpack_from("<I", stub, 24)[0]
    add("header.filetype", struct.unpack_from("<I", arm, 12) == struct.unpack_from("<I", stub, 12),
        str(struct.unpack_from("<I", arm, 12)[0]), str(struct.unpack_from("<I", stub, 12)[0]))
    add("header.flags", af & MH_POLICY_FLAGS == sf & MH_POLICY_FLAGS, hex(af & MH_POLICY_FLAGS),
        hex(sf & MH_POLICY_FLAGS), "PIE, ALLOW_STACK_EXECUTION, NO_HEAP_EXECUTION")
    # copy rules
    abv, sbv = cmd_bytes(arm, 0x32), cmd_bytes(stub, 0x32)
    if abv:
        a, s = abv[0], (sbv[0] if sbv else b"")
        for field, lo, hi in (("build.platform", 8, 12), ("build.minos", 12, 16), ("build.sdk", 16, 20)):
            add(field, a[lo:hi] == s[lo:hi], build_version_text(a), build_version_text(s))
        add("build.tools", a[20:] == s[20:], build_version_text(a), build_version_text(s))
    else:
        vm = cmd_bytes(arm, 0x24)
        if vm and sbv:
            v, sdk = struct.unpack_from("<II", vm[0], 8)
            add("build.version_min", struct.unpack_from("<III", sbv[0], 8) == (1, v, sdk),
                f"macos {ver(v)} sdk {ver(sdk)}", build_version_text(sbv[0]))
    a, s = cmd_bytes(arm, 0x2A), cmd_bytes(stub, 0x2A)
    add("source_version", a == s, source_version_text(a[0] if a else b""), source_version_text(s[0] if s else b""))
    ar = [lc_str(b) for b in cmd_bytes(arm, 0x8000001C)]
    sr = [lc_str(b) for b in cmd_bytes(stub, 0x8000001C)]
    own = [r for r in sr if r not in ar] if stub_own_rpaths is None else stub_own_rpaths
    want = own + [r for r in ar if r not in own]
    add("rpath", sr == want, ";".join(ar) or "-", ";".join(sr) or "-",
        "the stub's own rpaths, then the program's in order")
    a, s = [lc_str(b) for b in cmd_bytes(arm, 0x27)], [lc_str(b) for b in cmd_bytes(stub, 0x27)]
    add("dyld_environment", a == s, ";".join(a) or "-", ";".join(s) or "-")
    a, s = [lc_str(b) for b in cmd_bytes(arm, 0xE)], [lc_str(b) for b in cmd_bytes(stub, 0xE)]
    add("dylinker", a == s, ";".join(a), ";".join(s))
    # derive rules
    a, s = cmd_bytes(arm, 0x80000028), cmd_bytes(stub, 0x80000028)
    ast = struct.unpack_from("<Q", a[0], 16)[0] if a else None
    sst = struct.unpack_from("<Q", s[0], 16)[0] if s else None
    add("main.stacksize", ast == sst, str(ast), str(sst))
    want_uuid = embed.stub_uuid(arm)
    su = embed.macho_uuid(stub)
    add("uuid", su == want_uuid, want_uuid.hex(), su.hex() if su else "-", "v5 of the arm slice's LC_UUID")
    asg, ssg = segments(arm), segments(stub)
    add("pagezero", asg.get("__PAGEZERO", (None,))[0] == ssg.get("__PAGEZERO", (None,))[0],
        str(asg.get("__PAGEZERO", (None,))[0]), str(ssg.get("__PAGEZERO", (None,))[0]))
    carried = embed.carried_sections(arm)
    missing, wrong, prot = [], [], []
    for (seg, sect), (blob, maxprot, initprot) in sorted(carried.items()):
        st = ssg.get(seg)
        if st is None or sect not in st[3]:
            missing.append(f"{seg},{sect}")
            continue
        foff, ssize, _ = st[3][sect]
        if bytes(stub[foff:foff + ssize]) != blob:
            wrong.append(f"{seg},{sect}")
        if seg != "__TEXT" and (st[1], st[2]) != (maxprot, initprot):
            prot.append(f"{seg} {maxprot}/{initprot} vs {st[1]}/{st[2]}")
    add("sections", not (missing or wrong or prot), ";".join("%s,%s" % k for k in sorted(carried)) or "-",
        "missing " + ";".join(missing) + " differ " + ";".join(wrong) + " prot " + ";".join(prot)
        if missing or wrong or prot else "same", "carried sections: bytes and segment protections")
    # dylibs
    stub_deps = embed.dylib_commands(stub)
    own_names = ("@rpath/libhao.dylib", "@rpath/libhao_identity.dylib")
    sdeps = [d for d in stub_deps if d[1] not in own_names and not d[1].endswith("/libhao.dylib")
             and not d[1].endswith("/libhao_identity.dylib")]
    if x86 is not None and src_path:
        own = {n for _, n, _, _ in stub_deps if n not in {d[1] for d in sdeps}}
        links, skipped, _ = embed.native_deps(src_path, arm, x86, own)
        sd, ln = [tuple(d) for d in sdeps], [tuple(x) for x in links]
        first = next((f"{x} vs {y}" for x, y in zip(ln, sd) if x != y), "") if sd != ln else ""
        add("dylibs", sd == ln, f"{len(links)} native", f"{len(sdeps)} linked",
            "native_deps of the arm slice, in order, kind and versions" + (": first " + first if first else ""))
    else:
        adeps = [("weak" if k == "weak" else "load", n, c, v) for k, n, c, v in embed.dylib_commands(arm)]
        it = iter(adeps)
        ok = all(any(d == x for x in it) for d in sdeps)
        add("dylibs", ok, f"{len(adeps)} in the arm slice", f"{len(sdeps)} linked",
            "a subsequence of the arm slice's, same kind and versions")
    # signature
    ai, si = hao_codesig.info(arm), hao_codesig.info(stub)
    aid, aflags = cd_fields(arm)
    sid, sflags = cd_fields(stub)
    add("sig.identifier", aid == sid or (aid is None and sid), aid or "-", sid or "-")
    aent, sent = ai["entitlements"], si["entitlements"]
    if host_test:
        aent = embed.public_only(aent)
    extra = {k: v for k, v in sent.items() if k not in aent}
    lost = [k for k, v in aent.items() if sent.get(k) != v]
    add("sig.entitlements", not lost and all(embed.JIT_ENTITLEMENTS.get(k) == v for k, v in extra.items()),
        f"{len(aent)} keys", f"{len(sent)} keys", ("lost " + ",".join(lost) if lost else "") +
        (" extra " + ",".join(sorted(extra)) if extra else ""))
    want_rt = hao_codesig.dyld_env_blocked(ai)
    add("sig.runtime", bool(sflags & CS_RUNTIME) == bool(want_rt or aflags & CS_RUNTIME), hex(aflags), hex(sflags),
        "hardened runtime when dyld ignores DYLD_* for the program natively")
    for bit, nm in CS_COPIED_FLAGS.items():
        add("sig." + nm, bool(aflags & bit) == bool(sflags & bit), hex(aflags), hex(sflags))
    add("sig.library", bool(aflags & CS_REQUIRE_LV) == bool(sflags & CS_REQUIRE_LV), hex(aflags), hex(sflags),
        known=DENIED_FIELDS["sig.library"])
    add("sig.team", ai["team"] == si["team"], ai["team"] or "-", si["team"] or "-", known=DENIED_FIELDS["sig.team"])
    add("sig.platform", ai["platform"] == si["platform"], str(ai["platform"]), str(si["platform"]),
        known=DENIED_FIELDS["sig.platform"])
    for slot, nm in ((1, "sig.slot-1"), (3, "sig.slot-3")):
        a_, s_ = hao_codesig.special_slots(arm, slot), hao_codesig.special_slots(stub, slot)
        common = set(a_) & set(s_)
        ok = bool(a_) == bool(s_) and all(a_[t] == s_[t] for t in common) and (not a_ or common)
        add(nm, ok, ",".join(f"{t}:{h.hex()[:16]}" for t, h in sorted(a_.items())) or "-",
            ",".join(f"{t}:{h.hex()[:16]}" for t, h in sorted(s_.items())) or "-")
    return rows


def pairs(a):
    """[(label, src_path or None, arm bytes, stub bytes)] from the command line."""
    embed = None
    out = []
    if a.stubs:
        m = json.load(open(os.path.join(a.stubs, "manifest.json")))
        embed = load_embed()
        for e in m["files"]:
            src = os.path.join(a.root, e["path"])
            arm = embed.arm_slice(open(src, "rb").read())
            out.append((e["path"], src, arm, open(os.path.join(a.stubs, e["stub"]), "rb").read()))
        return out
    embed = load_embed()
    for f in a.files:
        data = open(f, "rb").read()
        arm = embed.arm_slice(data)
        x = [p for c, _, p in hao_fat.slices(data) or [] if c == hao_fat.CPU_X86_64]
        if arm is None or not x:
            raise SystemExit(f"stub_identity_gate: {f}: not an embedded file (needs an arm64 and an x86_64 slice)")
        out.append((f, f, arm, x[0]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stubs")
    ap.add_argument("--root")
    ap.add_argument("--x86-cache")
    ap.add_argument("--host-test", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--rules", action="store_true", help="print the rule table as TSV and exit")
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    if a.rules:
        print("cmd\tname\taction\treason")
        for c, (n, act, why) in sorted(RULES.items(), key=lambda kv: (kv[1][1], kv[1][0])):
            print(f"0x{c:x}\t{n}\t{act}\t{why}")
        for f, why in DENIED_FIELDS.items():
            print(f"-\t{f}\tdeny\t{why}")
        return 0
    if bool(a.stubs) != bool(a.root) or (not a.stubs and not a.files):
        ap.error("give FAT... or --stubs DIR --root ROOT")
    embed = load_embed()
    x86 = embed.X86Libraries(a.x86_cache, a.root or "/") if a.x86_cache else None
    try:
        progs = pairs(a)
    except (OSError, ValueError, KeyError) as e:
        print(f"stub_identity_gate: {e}", file=sys.stderr)
        return 2
    counts = collections.Counter()
    diffs, per = [], []
    for label, src, arm, stub in progs:
        try:
            rows = compare(arm, stub, embed, src, x86, a.host_test)
        except (ValueError, struct.error, IndexError) as e:
            rows = [("parse", "DIFF", "-", "-", str(e))]
        nd = sum(r[1] == "DIFF" for r in rows)
        per.append((label, nd, ",".join(r[0] for r in rows if r[1] == "DIFF") or "-"))
        for r in rows:
            counts[(r[0], r[1])] += 1
            if r[1] in ("DIFF", "KNOWN"):
                diffs.append((label,) + r)
    fields = sorted({f for f, _ in counts})
    lines = [f"# stub identity gate: {len(progs)} programs, {sum(1 for p in per if p[1])} with a DIFF", "",
             "| field | SAME | DIFF | KNOWN | DENY |", "|---|---|---|---|---|"]
    for f in fields:
        lines.append(f"| {f} | " + " | ".join(str(counts[(f, v)]) for v in ("SAME", "DIFF", "KNOWN", "DENY")) + " |")
    summary = "\n".join(lines) + "\n"
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "diff.tsv"), "w") as f:
            f.write("program\tfield\tverdict\tarm\tstub\tnote\n")
            for d in diffs:
                f.write("\t".join(str(x).replace("\t", " ").replace("\n", " ") for x in d) + "\n")
        with open(os.path.join(a.out, "programs.tsv"), "w") as f:
            f.write("program\tdiffs\tfields\n")
            for p in per:
                f.write("\t".join(map(str, p)) + "\n")
        with open(os.path.join(a.out, "summary.md"), "w") as f:
            f.write(summary)
    sys.stdout.write(summary)
    if not a.out:
        for d in diffs:
            if d[2] == "DIFF":
                print("DIFF\t" + "\t".join(map(str, (d[0], d[1], d[3], d[4], d[5]))))
    return 1 if any(p[1] for p in per) else 0


if __name__ == "__main__":
    sys.exit(main())
