"""hao-install.py SYSTEM PAYLOAD: install hao into a macOS System volume.

Runs as root inside an image builder (pure Python, no developer tools):
  1. PAYLOAD/root/** -> SYSTEM (usr/libexec/hao, usr/lib/hao/..., among them
     usr/lib/hao/trusted-cdhashes, which hao trusts only root-owned and not
     writable by group or others), root:wheel, modes as in the payload.
  2. For every PAYLOAD/embed/manifest.json entry whose file in SYSTEM has the
     recorded source hash, add the signed x86_64 stub slice in place (same
     inode, owner, mode and flags). An entry with "mode": "translate" is a
     universal program: its own x86_64 slices are replaced by the stub. A file
     that already has the recorded result hash counts as embedded. Other files
     that already carry an x86_64 slice (kept from another release) are
     reported and left alone; any other mismatch, or any mismatch of a
     translate entry, is an error. A stub of a bundle's main executable binds
     the bundle's Info.plist and resource seal (code-signature special slots
     -1 and -3, the entry's "bound" list): it is embedded only if SYSTEM's
     files have exactly the hashes the stub binds (check_bound); otherwise
     it would carry another bundle identifier than the program.
  3. PAYLOAD/replace/manifest.json, if the payload has one: whole-file fixes
     (e.g. 27.0 x86_64 dyld's PID-1 boot-spec count), each applied only to the
     exact source hash. Without one, no other system file is touched.
  4. The AOT cache directories hao trusts for root, PID 1 and daemons:
     usr/lib/hao/aot (sealed system volume; what PID 1 can read) and
     private/var/db/hao (only when SYSTEM has private/var/db), each root:wheel
     0755, with any cache files in them root:wheel 0644. hao never loads a
     cache from a directory or file that others can write, or that is owned
     by anyone but root (src/aot_policy.c); with neither directory it runs
     without AOT. A symlink or non-directory at either path is an error.
     The payload's caches (hao-image-payload --aot) are bound to its
     libhao.dylib: a <uuid>.aot in usr/lib/hao/aot that is not a v7 cache of
     the installed usr/lib/hao/libhao.dylib (left by an earlier payload) is
     removed, since hao would only read and reject it; install.json
     aot_caches records the count, bytes and removals.
  5. Writes SYSTEM/usr/lib/hao/install.json.
Every name a manifest supplies ("path", "stub", "file", a bound "file") is a
relative path under SYSTEM or PAYLOAD by the rule of tools/hao_release.py
(relative, safe): not absolute, no "..", no backslash or NUL, already
normalised. One that is not fails the install before anything is written.
No such name, and no file of PAYLOAD/root, is read or written through a
symbolic link: a link among the directories of the path, in PAYLOAD/root,
or where a file is to be written is an error, found before the first write
where the payload alone decides it (a link in place of a program to embed
counts as a missing program, as before).
Exit 1 on any error or if a path the payload requires was not embedded: the
manifest's "required" list (empty for a runtime-only payload), or for
payloads that predate it, sbin/launchd and usr/libexec/xpcproxy.
"""
import hashlib
import json
import os
import posixpath
import shutil
import stat
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hao_codesig  # noqa: E402
import hao_fat  # noqa: E402

LEGACY_REQUIRED = ["sbin/launchd", "usr/libexec/xpcproxy"]


class Unsafe(Exception):
    """A name or file the installer refuses: the install fails."""


def relative(name):
    """NAME's components if it is a relative path by the rule of
    hao_release.relative (this file ships to the image builder without
    hao_release.py, so the rule is repeated; tests/tools/test_install.py holds
    the two to the same answers): a non-empty string, not absolute, without
    "..", backslash or NUL, and already normalised. Unsafe otherwise."""
    if not isinstance(name, str) or not name:
        raise Unsafe("empty or non-string path %r" % (name,))
    if name.startswith("/") or "\\" in name or "\0" in name or name == "." or \
            ".." in name.split("/") or posixpath.normpath(name) != name:
        raise Unsafe("unsafe path %r (must be relative, normalised, without ..)" % name)
    return name.split("/")


def inside(root, name, final_link=False):
    """ROOT/NAME for a relative NAME none of whose components under ROOT is a
    symbolic link; Unsafe otherwise. final_link=True leaves the last
    component to the caller (which must not follow it)."""
    parts = relative(name)
    p = root
    for k, part in enumerate(parts):
        p = os.path.join(p, part)
        if os.path.islink(p) and not (final_link and k == len(parts) - 1):
            raise Unsafe("%s: %s is a symbolic link" % (name, "/".join(parts[:k + 1])))
    return p


def open_nofollow(path, create=False):
    """PATH opened for writing in place (same inode), never through a
    symbolic link and only if it is a regular file; Unsafe otherwise."""
    flags = os.O_WRONLY | os.O_NOFOLLOW | os.O_NONBLOCK | (os.O_CREAT if create else 0)
    try:
        fd = os.open(path, flags, 0o644)
    except OSError as x:
        raise Unsafe("%s: not written (%s)" % (path, "a symbolic link" if os.path.islink(path) else x.strerror))
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        raise Unsafe("%s: not written (not a regular file)" % path)
    return os.fdopen(fd, "wb")


def install_tree(src, dst):
    """Copy the regular files of SRC onto DST. The whole tree is checked
    before the first write: Unsafe for a symbolic link or special file in
    SRC, or a symbolic link or non-directory/non-file in the way in DST."""
    if os.path.islink(src) or not os.path.isdir(src):
        raise Unsafe("payload root is not a directory")
    new_dirs, copies = [], []
    for base, dirs, files in os.walk(src):
        rel = os.path.relpath(base, src)
        for name in dirs + files:
            s = os.path.join(base, name)
            r = name if rel == "." else os.path.join(rel, name)
            d = inside(dst, r)
            if os.path.islink(s):
                raise Unsafe("payload root/%s is a symbolic link" % r)
            if name in dirs:
                if os.path.lexists(d) and not os.path.isdir(d):
                    raise Unsafe("%s exists and is not a directory" % r)
                new_dirs.append(d)
            else:
                if not os.path.isfile(s) or (os.path.lexists(d) and not os.path.isfile(d)):
                    raise Unsafe("%s is not a regular file (payload or SYSTEM)" % r)
                copies.append((s, d))
    root = os.geteuid() == 0
    for d in new_dirs:
        if not os.path.isdir(d):
            os.mkdir(d, 0o755)
            if root:
                os.chown(d, 0, 0)
    for s, d in copies:
        with open(s, "rb") as fs, open_nofollow(d, create=True) as fd:
            fd.truncate()
            shutil.copyfileobj(fs, fd)
            os.fchmod(fd.fileno(), os.stat(s).st_mode & 0o7777)
            if root:
                os.fchown(fd.fileno(), 0, 0)
    return len(copies)


def manifest_names(embed, replace):
    """Check every path the manifests name (relative) before the install
    touches SYSTEM; Unsafe names the first bad one."""
    for what, man, keys in (("embed", embed, ("path", "stub")), ("replace", replace, ("path", "file"))):
        try:
            for e in man["files"] if man is not None else []:
                for k in keys:
                    relative(e[k])
                for b in e.get("bound", []):
                    if "file" in b:
                        relative(b["file"])
        except Unsafe as x:
            raise Unsafe("%s/manifest.json: %s" % (what, x))
        except (KeyError, TypeError, AttributeError) as x:
            raise Unsafe("%s/manifest.json: malformed (%s: %s)" % (what, type(x).__name__, x))


AOT_DIRS = ["usr/lib/hao/aot", "private/var/db/hao"]


def ensure_aot_dirs(system, errors):
    """Create or fix the root-owned AOT cache directories; returns a record."""
    done = []
    root = os.geteuid() == 0
    for rel in AOT_DIRS:
        d = os.path.join(system, rel)
        parent = os.path.dirname(d)
        try:
            inside(system, os.path.dirname(rel))
        except Unsafe as x:
            errors.append("%s (%s)" % (rel, x))
            continue
        if not os.path.isdir(parent) or os.path.islink(parent):
            done.append(rel + " (skipped: no " + os.path.relpath(parent, system) + ")")
            continue
        if os.path.islink(d) or (os.path.exists(d) and not os.path.isdir(d)):
            errors.append(rel + " (exists and is not a directory)")
            continue
        if not os.path.isdir(d):
            os.mkdir(d, 0o755)
        os.chmod(d, 0o755)
        if root:
            os.chown(d, 0, 0)
        for name in os.listdir(d):
            f = os.path.join(d, name)
            if os.path.islink(f) or not os.path.isfile(f):
                errors.append(os.path.join(rel, name) + " (not a regular file)")
                continue
            os.chmod(f, 0o644)
            if root:
                os.chown(f, 0, 0)
        st = os.stat(d)
        done.append("%s (uid %d, mode %o)" % (rel, st.st_uid, st.st_mode & 0o7777))
    return done


def macho_uuid(path):
    """LC_UUID (hex) of a thin 64-bit Mach-O, or None."""
    try:
        mo = open(path, "rb").read()
        if mo[:4] != b"\xcf\xfa\xed\xfe":
            return None
        for cmd, off, size in hao_fat.load_commands(mo, path):
            if cmd == 0x1B:   # LC_UUID
                return mo[off + 8:off + 24].hex() if size >= 24 else None
    except (OSError, ValueError):
        pass
    return None


def check_aot_caches(system, errors):
    """The caches in usr/lib/hao/aot (include/hao/aot.h: magic, version 7, the
    guest UUID at 16, the generating libhao/CPU profile at 32 and the kind of code
    identity at 96, in a 136-byte header): keep those of the installed libhao
    that are bound to a signed guest's cdhash (kind 1; hao serves no other
    kind from a system directory), remove the rest; returns a record."""
    d = os.path.join(system, AOT_DIRS[0])
    gen = macho_uuid(os.path.join(system, "usr/lib/hao/libhao.dylib"))
    gen_profiles = set()
    if gen is not None:
        aes_gen = bytearray.fromhex(gen)
        aes_gen[15] ^= 1
        gen_profiles = {gen, aes_gen.hex()}  # jit.c's only two CPU profiles
    rec = {"libhao_uuid": gen, "caches": 0, "bytes": 0, "removed_stale": []}
    if not os.path.isdir(d) or os.path.islink(d):
        return rec
    for name in sorted(os.listdir(d)):
        if len(name) != 36 or not name.endswith(".aot") or any(c not in "0123456789abcdef" for c in name[:32]):
            continue
        f = os.path.join(d, name)
        if os.path.islink(f) or not os.path.isfile(f):
            continue   # ensure_aot_dirs reported it
        with open(f, "rb") as fh:
            h = fh.read(136)
        if len(h) == 136 and h[:8] == b"HAOAOT2\0" and int.from_bytes(h[8:12], "little") == 7 and \
                int.from_bytes(h[12:16], "little") == 136 and int.from_bytes(h[96:100], "little") == 1 and \
                h[16:32].hex() == name[:32] and gen is not None and h[32:48].hex() in gen_profiles:
            rec["caches"] += 1
            rec["bytes"] += os.path.getsize(f)
        else:
            os.unlink(f)
            rec["removed_stale"].append(name)
    if gen is None and rec["removed_stale"]:
        errors.append("usr/lib/hao/libhao.dylib has no LC_UUID: AOT caches removed")
    return rec


def check_bound(system, e, stub):
    """None if the stub's special slots -1 (Info.plist) and -3 (resource
    seal) are what the manifest entry's "bound" list says and every bound file
    in SYSTEM has that hash, else why not. Entries that predate the list are
    accepted only if their stub binds no resource seal (they bound at most a
    tool's own __info_plist section)."""
    try:
        listed = {-b["slot"]: b for b in e.get("bound", [])}
        for slot in (hao_codesig.CSSLOT_INFOSLOT, hao_codesig.CSSLOT_RESOURCEDIR):
            have = hao_codesig.special_slots(stub, slot)
            b = listed.get(slot)
            if "bound" not in e:
                if have and slot == hao_codesig.CSSLOT_RESOURCEDIR:
                    return "stub binds a resource seal the manifest does not name"
                continue
            if bool(have) != bool(b):
                return "stub slot -%d %s but the manifest says %s" % (
                    slot, "is bound" if have else "is empty", "bound" if b else "not bound")
            if not b or "section" in b:
                continue   # nothing bound, or the stub's own carried __info_plist (in result_sha256)
            f = inside(system, b["file"])
            if not os.path.isfile(f):
                return "bound %s missing" % b["file"]
            blob = open(f, "rb").read()
            if hashlib.sha256(blob).hexdigest() != b["sha256"] or \
                    any(hao_codesig.digest(t, blob) != h for t, h in have.items()):
                return "bound %s differs from the one the stub binds" % b["file"]
    except Unsafe as x:
        return str(x)
    except (KeyError, TypeError, ValueError) as x:
        return "bad bound entry or stub signature: %s" % x
    return None


def write_in_place(path, data):
    with open_nofollow(path) as f:
        f.write(data)
        f.truncate()
        f.flush()
        os.fsync(f.fileno())


def main():
    try:
        return install(os.path.realpath(sys.argv[1]), os.path.realpath(sys.argv[2]))
    except Unsafe as x:
        print("hao: error: install refused:", x, flush=True)
        return 1


def install(system, payload):
    manifest = json.load(open(os.path.join(payload, "embed", "manifest.json")))
    rman = os.path.join(payload, "replace", "manifest.json")
    replace = json.load(open(rman)) if os.path.exists(rman) else None
    manifest_names(manifest, replace)
    record_path = inside(system, "usr/lib/hao/install.json")
    installed = install_tree(os.path.join(payload, "root"), system)
    done, already, native, missing, errors, bound = [], [], [], [], [], []
    for e in manifest["files"]:
        if e["path"] in done or e["path"] in missing:
            continue  # listed twice (payloads built before hao-embed skipped repeats)
        try:
            target = inside(system, e["path"], final_link=True)
            stub_path = inside(os.path.join(payload, "embed"), e["stub"])
        except Unsafe as x:
            errors.append("%s (%s; not embedded)" % (e["path"], x))
            continue
        if not os.path.isfile(target) or os.path.islink(target):
            missing.append(e["path"])
            continue
        data = open(target, "rb").read()
        translate = e.get("mode") == "translate"
        stub = open(stub_path, "rb").read()
        why = check_bound(system, e, stub)
        if why:
            errors.append(e["path"] + " (" + why + "; not embedded)")
            continue
        if any("file" in b for b in e.get("bound", [])):
            bound.append(e["path"])   # a bundle's main executable: Info.plist and seal bound
        digest = hashlib.sha256(data).hexdigest()
        if digest == e["result_sha256"]:
            done.append(e["path"])
            already.append(e["path"])
            continue
        if digest != e["source_sha256"]:
            if translate:
                errors.append(e["path"] + " (source hash differs; not translated)")
            else:
                (native if hao_fat.has_cpu(data, hao_fat.CPU_X86_64) else errors).append(e["path"])
            continue
        merged = hao_fat.combine(data, stub, drop_x86_64=translate)
        if hashlib.sha256(merged).hexdigest() != e["result_sha256"]:
            errors.append(e["path"] + " (merge result differs)")
            continue
        write_in_place(target, merged)
        done.append(e["path"])
    replaced = []
    for e in (replace["files"] if replace is not None else []):
        try:
            target = inside(system, e["path"])
            new_path = inside(os.path.join(payload, "replace"), e["file"])
        except Unsafe as x:
            errors.append("%s (%s; not replaced)" % (e["path"], x))
            continue
        data = open(target, "rb").read() if os.path.isfile(target) else b""
        new = open(new_path, "rb").read()
        if hashlib.sha256(new).hexdigest() != e["result_sha256"]:
            errors.append(e["path"] + " (replacement file corrupt)")
        elif hashlib.sha256(data).hexdigest() == e["result_sha256"]:
            replaced.append(e["path"])
        elif hashlib.sha256(data).hexdigest() != e["source_sha256"]:
            errors.append(e["path"] + " (source hash differs; fix not applicable)")
        else:
            write_in_place(target, new)
            replaced.append(e["path"])
    aot_dirs = ensure_aot_dirs(system, errors)
    aot_caches = check_aot_caches(system, errors)
    required = manifest.get("required", LEGACY_REQUIRED)
    for r in required:
        if r not in done:
            errors.append(r + " (required, not embedded)")
    record = {"installed_files": installed, "embedded": len(done), "already_embedded": len(already),
              "required": required, "kept_native": native,
              "bundle_bound": sum(p in done for p in bound),
              "missing": missing, "replaced": replaced, "aot_dirs": aot_dirs, "aot_caches": aot_caches, "errors": errors,
              "stub_sha256": manifest["stub_sha256"]}
    with open_nofollow(record_path, create=True) as f:
        f.truncate()
        f.write((json.dumps(record, indent=1) + "\n").encode())
        os.fchmod(f.fileno(), 0o644)
    print(f"hao: installed {installed} runtime files; embedded {len(done)} executables; "
          f"{sum(p in done for p in bound)} bind their bundle's Info.plist; kept {len(native)} native; {len(missing)} missing; replaced {replaced}; AOT dirs {aot_dirs}; "
          f"AOT caches {aot_caches['caches']} ({aot_caches['bytes'] >> 20} MB, {len(aot_caches['removed_stale'])} stale removed); "
          f"{len(errors)} errors", flush=True)
    for x in errors[:20]:
        print("hao: error:", x, flush=True)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
