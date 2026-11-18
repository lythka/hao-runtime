"""hao-install.py SYSTEM PAYLOAD: install hao into a macOS System volume.

Runs as root inside an image builder (pure Python, no developer tools):
  0. PAYLOAD must hold exactly what PAYLOAD/payload-manifest.json lists
     (tools/hao-image-payload writes it; payload_inventory): a file it does
     not list, a symbolic link or special file anywhere in PAYLOAD, a listed
     file that is missing, or a file under root/ with a set-ID or sticky bit
     refuses the install before anything is written. Listed AOT caches
     (root/usr/lib/hao/aot/) may be absent: a payload remade on the user's
     Mac has none, and `hao --aot` makes them after the first boot. Every
     file the install reads from PAYLOAD (root/, the manifests, the stubs
     and replacements they name) is then copied once into a private
     directory (Staging: used through a descriptor, each copy checked by
     inode when read back) and its sha256 checked against
     payload-manifest.json; only those copies are read afterwards. Every
     other listed file is hashed too, and two root/ names that are one file
     on a case-insensitive volume (or a file and a directory) are refused.
  1. The listed PAYLOAD/root/** files -> SYSTEM (usr/libexec/hao,
     usr/lib/hao/..., among them usr/lib/hao/trusted-cdhashes, which hao
     trusts only root-owned and not writable by group or others), root:wheel,
     0755 if the payload's file has an execute bit, else 0644, each written
     to a new file and renamed over the old one through directory
     descriptors (another hard link to the old file keeps its bytes).
  2. For every PAYLOAD/embed/manifest.json entry whose file in SYSTEM has the
     recorded source hash, add the signed x86_64 stub slice in place (same
     inode, owner, mode and flags). An entry with "mode": "translate" is a
     universal program: its own x86_64 slices are replaced by the stub. A file
     that already has the recorded result hash counts as embedded. A file
     whose x86_64 slice is a hao stub of another build (it links
     libhao.dylib; hao's own launcher, with __TEXT,__hao_cli, is not a stub)
     gets this payload's stub instead when that gives the recorded result
     ("restubbed"). Other files that already carry an x86_64 slice (kept from
     another release) are reported and left alone, unless their x86_64 slice
     is a hao stub that does not name the file's arm slice in a
     __TEXT,__hao_slice record (stub_binding): such a legacy stub is an
     error. Any other mismatch, or any mismatch of a translate entry, is an
     error. A hao stub of the payload must name the arm slice it is embedded
     with. A stub of a bundle's main executable binds
     the bundle's Info.plist and resource seal (code-signature special slots
     -1 and -3, the entry's "bound" list): it is embedded only if SYSTEM's
     files have exactly the hashes the stub binds (check_bound); otherwise
     it would carry another bundle identifier than the program.
  3. PAYLOAD/replace/manifest.json, if the payload has one: whole-file fixes
     (e.g. 27.0 x86_64 dyld's PID-1 boot-spec count), each applied only to the
     exact source hash and never to a file with another hard link. Without
     one, no other system file is touched. Programs to embed and files to
     replace are opened through directory descriptors that follow no link,
     read and written through that one descriptor.
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
  5. Every fat file in SYSTEM whose x86_64 slice is a hao stub must name its
     arm slice (legacy_stubs): one that does not is an error, wherever it is.
  6. Writes SYSTEM/usr/lib/hao/install.json.
Every name a manifest supplies ("path", "stub", "file", a bound "file") is a
relative path under SYSTEM or PAYLOAD by the rule of tools/hao_release.py
(relative, safe): not absolute, no "..", no backslash or NUL, already
normalised. One that is not fails the install before anything is written.
No such name, and no file of PAYLOAD, is read or written through a
symbolic link: a link among the directories of the path, in PAYLOAD,
or where a file is to be written is an error, found before the first write
where the payload alone decides it (a link in place of a program to embed
counts as a missing program, as before).
Exit 1 on any error or if a path the payload requires was not embedded: the
manifest's "required" list (empty for a runtime-only payload), or for
payloads that predate it, sbin/launchd and usr/libexec/xpcproxy.
tools/hao-runtime-on-mac --install-live imports the steps (load_payload,
install_root, embed_programs, replace_files, check_aot_caches, install_record,
write_record) to update an installed system with these same checks.
"""
import hashlib
import io
import json
import os
import posixpath
import shutil
import stat
import struct
import sys
import tempfile
import unicodedata

# run from the payload it installs: importing its helpers must not write __pycache__ into it (an unlisted file)
sys.dont_write_bytecode = True
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


MANIFEST = "payload-manifest.json"
# Listed by the payload build, made on the installed system when absent: tools/hao-public-archive leaves the AOT
# caches out, and `hao --aot` makes them after the first boot.
MADE_LATER = "root/usr/lib/hao/aot/"


def read_nofollow(path):
    """The bytes of the regular file PATH, read through one descriptor that follows no symbolic link."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise OSError("not a regular file")
        with os.fdopen(fd, "rb", closefd=False) as f:
            return f.read()
    finally:
        os.close(fd)


def payload_inventory(payload):
    """{relative path: (sha256, mode)} of the files PAYLOAD holds, once it holds exactly what MANIFEST lists:
    Unsafe for a file it does not list, a symbolic link or special file anywhere, a listed file that is missing
    (an AOT cache under MADE_LATER may be), or a file under root/ with a set-ID or sticky bit."""
    if os.path.islink(payload) or not os.path.isdir(payload):
        raise Unsafe("payload is not a directory")
    try:
        files = json.loads(read_nofollow(os.path.join(payload, MANIFEST)))["files"]
        listed = {relative(r) and r: d for r, d in files.items()}
    except Unsafe as x:
        raise Unsafe("%s: %s" % (MANIFEST, x))
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as x:
        raise Unsafe("%s: missing or malformed (%s)" % (MANIFEST, x))
    for r, d in listed.items():
        if r == MANIFEST or not isinstance(d, str) or len(d) != 64 or d.strip("0123456789abcdef"):
            raise Unsafe("%s: bad entry %r" % (MANIFEST, r))
    have = {}
    for base, dirs, names in os.walk(payload):
        for name in sorted(dirs + names):
            p = os.path.join(base, name)
            rel = os.path.relpath(p, payload)
            st = os.lstat(p)
            if stat.S_ISDIR(st.st_mode) or rel == MANIFEST:
                continue
            if not stat.S_ISREG(st.st_mode):
                raise Unsafe("payload %s is %s" % (rel, "a symbolic link" if stat.S_ISLNK(st.st_mode) else
                                                   "not a regular file"))
            if rel not in listed:
                raise Unsafe("payload %s is not in %s" % (rel, MANIFEST))
            if rel.startswith("root/") and st.st_mode & (stat.S_ISUID | stat.S_ISGID | stat.S_ISVTX):
                raise Unsafe("payload %s has a set-ID or sticky bit (mode %o)" % (rel, st.st_mode & 0o7777))
            have[rel] = (listed[rel], st.st_mode)
    for r in sorted(set(listed) - set(have)):
        if not r.startswith(MADE_LATER):
            raise Unsafe("payload %s is listed in %s but missing" % (r, MANIFEST))
    return have


class Staging:
    """A private directory (mkdtemp: mode 0700, this user's) for copies of payload files, used only through a
    descriptor of it, so renaming or replacing its path changes nothing. Each copy is created exclusively and
    remembered by inode; read() opens it again through the descriptor and refuses anything else (another inode,
    another owner, a second link)."""

    def __init__(self):
        self.path = tempfile.mkdtemp(prefix="hao-install.")
        self.fd = os.open(self.path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        self.files = {}   # rel -> (name, dev, ino)

    def close(self):
        os.close(self.fd)
        shutil.rmtree(self.path, ignore_errors=True)

    def __contains__(self, rel):
        return rel in self.files

    def __iter__(self):
        return iter(self.files)

    def add(self, rel, src):
        """Copy the open file SRC as REL; returns the sha256 of what was copied."""
        name = "%d" % len(self.files)
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)
        h = hashlib.sha256()
        with os.fdopen(fd, "wb") as fc:
            for block in iter(lambda: src.read(1 << 20), b""):
                h.update(block)
                fc.write(block)
            st = os.fstat(fc.fileno())
        self.files[rel] = (name, st.st_dev, st.st_ino)
        return h.hexdigest()

    def open(self, rel):
        name, dev, ino = self.files[rel]
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=self.fd)
        st = os.fstat(fd)
        if (st.st_dev, st.st_ino) != (dev, ino) or st.st_uid != os.geteuid() or st.st_nlink != 1:
            os.close(fd)
            raise Unsafe("the private copy of payload %s was changed" % rel)
        return os.fdopen(fd, "rb")

    def read(self, rel):
        with self.open(rel) as f:
            return f.read()


def stage(payload, admitted, rels, out):
    """Add to OUT (a Staging) each of RELS not yet in it (a file ADMITTED by payload_inventory): read once through
    a descriptor that follows no link and checked against its sha256 in MANIFEST; Unsafe for a file not admitted
    or not matching. Only the copies are read afterwards."""
    for rel in sorted(set(rels)):
        if rel in out:
            continue
        if rel not in admitted:
            raise Unsafe("payload %s is not in %s" % (rel, MANIFEST))
        try:
            fd = os.open(inside(payload, rel), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except OSError as x:
            raise Unsafe("payload %s: not read (%s)" % (rel, x.strerror))
        with os.fdopen(fd, "rb") as fs:
            if not stat.S_ISREG(os.fstat(fs.fileno()).st_mode):
                raise Unsafe("payload %s is not a regular file" % rel)
            digest = out.add(rel, fs)
        if digest != admitted[rel][0]:
            raise Unsafe("payload %s does not match its sha256 in %s" % (rel, MANIFEST))
    return out


def dir_at(top_fd, parts, create=False):
    """A descriptor of the directory TOP_FD/PARTS..., each component opened through its parent's descriptor
    without following a link (made 0755, root:wheel as root, where CREATE and missing) and owned by root or this
    user, writable by nobody else, with no ACL granting write (hao_codesig.dir_fd_unshared); Unsafe otherwise."""
    fd = os.dup(top_fd)
    for part in parts:
        try:
            nfd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
        except FileNotFoundError:
            if not create:
                os.close(fd)
                raise
            os.mkdir(part, 0o755, dir_fd=fd)
            if os.geteuid() == 0:
                os.chown(part, 0, 0, dir_fd=fd, follow_symlinks=False)
            nfd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
        except OSError as x:
            os.close(fd)
            raise Unsafe("%s: %s is a symbolic link or not a directory (%s)" % ("/".join(parts), part, x.strerror))
        os.close(fd)
        fd = nfd
        why = hao_codesig.dir_fd_unshared(fd, {0, os.geteuid()})
        if why:
            os.close(fd)
            raise Unsafe("%s: %s: %s, so others could replace what is written there" % ("/".join(parts), part, why))
    return fd


def publish(dfd, name, src, mode):
    """The bytes of the open file SRC as NAME in the directory DFD: written to a new file there (exclusive, no
    link followed), root:wheel as root, MODE, then renamed over NAME. NAME is replaced, never written through: a
    link or another hard link to the old file is left as it was."""
    for _ in range(100):
        tmp = ".hao-install.%s" % os.urandom(6).hex()
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=dfd)
            break
        except FileExistsError:
            continue
    with os.fdopen(fd, "wb") as f:
        shutil.copyfileobj(src, f)
        if os.geteuid() == 0:
            os.fchown(f.fileno(), 0, 0)
        os.fchmod(f.fileno(), mode)
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, name, src_dir_fd=dfd, dst_dir_fd=dfd)


def root_mode(admitted, rel):
    """The mode the payload's root/ file REL is installed with: 0755 if it has an execute bit, else 0644."""
    return 0o755 if admitted[rel][1] & 0o111 else 0o644


def install_root(admitted, staged, dst, only=None):
    """Install the staged root/ files (those in ONLY, if given) onto DST, root:wheel, 0755 or 0644 (the payload
    file's execute bit), making their directories and usr/lib/hao (the record's, the AOT caches' parent) 0755.
    Every path is checked before the first write (Unsafe for a symbolic link or a non-directory/non-file in the
    way); the writes then go through directory descriptors and replace each file by rename (publish)."""
    plan = []
    for rel in sorted(r for r in staged if r.startswith("root/") and (only is None or r in only)) + [None]:
        name = rel[len("root/"):] if rel else "usr/lib/hao/install.json"
        d = inside(dst, name)
        p = dst
        for part in relative(name)[:-1]:
            p = os.path.join(p, part)
            if os.path.lexists(p) and not os.path.isdir(p):
                raise Unsafe("%s exists and is not a directory" % os.path.relpath(p, dst))
        if os.path.lexists(d) and not os.path.isfile(d):
            raise Unsafe("%s is not a regular file (SYSTEM)" % name)
        plan.append((rel, relative(name)))
    top = os.open(dst, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    n = 0
    try:
        for rel, parts in plan:
            dfd = dir_at(top, parts[:-1], create=True)
            try:
                if rel:
                    with staged.open(rel) as src:
                        publish(dfd, parts[-1], src, root_mode(admitted, rel))
                    n += 1
            finally:
                os.close(dfd)
    finally:
        os.close(top)
    return n


LC_SEGMENT_64 = 0x19
# load commands that name a library (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB,
# LC_LOAD_UPWARD_DYLIB)
DYLIB_CMDS = (0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023)
X86_CPUS = (hao_fat.CPU_X86_64, 7)   # x86_64 (every subtype, x86_64h too) and i386
SLICE_MAGIC = b"haoslice"


def arm_slice(slices):
    """The arm64 slice hao runs (tools/hao-embed arm_slice): plain arm64e > other arm64e.* > arm64."""
    best, rank = None, 0
    for c, sub, payload in slices:
        if c == hao_fat.CPU_ARM64:
            r = (3 if sub & 0xFF000000 in (0, 0x80000000) else 2) if sub & 0x00FFFFFF == 2 else 1
            if r > rank:
                best, rank = payload, r
    return best


def _hao_slice(mo):
    """(is a hao executable, has __TEXT,__hao_cli, __hao_slice record or None) for the x86 slice MO. A hao
    executable is an executable that names libhao in any form: a library load command of any kind whose name is
    libhao*, or the bytes "libhao" anywhere (a dlopen of it)."""
    record, cli, libhao = None, False, b"libhao" in mo.lower()
    for cmd, off, size in hao_fat.load_commands(mo):
        if cmd in DYLIB_CMDS and size >= 24:
            name = mo[off + int.from_bytes(mo[off + 8:off + 12], "little"):off + size].split(b"\0")[0]
            libhao |= name.rsplit(b"/", 1)[-1].lower().startswith(b"libhao")
        elif cmd == LC_SEGMENT_64 and size >= 72 and mo[off + 8:off + 24].rstrip(b"\0") == b"__TEXT":
            for k in range(min(int.from_bytes(mo[off + 64:off + 68], "little"), (size - 72) // 80)):
                at = off + 72 + 80 * k
                sect = mo[at:at + 16].rstrip(b"\0")
                cli |= sect == b"__hao_cli"
                if sect == b"__hao_slice":
                    n, fo = struct.unpack_from("<QI", mo, at + 40)
                    record = mo[fo:fo + n] if n == 32 and fo + n <= len(mo) else b""
    return libhao, cli, record


def stub_binding(data, cli_ok=None):
    """None if no x86 slice of DATA (x86_64 of any subtype, x86_64h, i386: every one is examined) is a hao stub:
    an executable naming libhao (_hao_slice), other than hao's launcher. Otherwise "" if every such slice's
    __TEXT,__hao_slice record (tools/hao-embed slice_record) names the cdhash of DATA's arm slice, else why not:
    src/libval.c runs only that slice under the stub. A slice with the launcher's __TEXT,__hao_cli marker counts
    as the launcher only where CLI_OK(DATA) says this file is the payload's own launcher; anywhere else the
    marker proves nothing."""
    sl = hao_fat.slices(data) or []
    found, why = False, None
    for c, _, mo in sl:
        if c not in X86_CPUS:
            continue
        if mo[:4] not in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):
            return "an x86 slice that is not a Mach-O"
        if int.from_bytes(mo[12:16], "little") != 2:
            continue
        try:
            libhao, cli, record = _hao_slice(mo)
        except (ValueError, struct.error) as x:
            return "an unreadable x86 slice (%s)" % x
        if not libhao or (cli and cli_ok is not None and cli_ok(data)):
            continue
        found = True
        if record is None:
            why = why or "a hao stub without a __hao_slice record (legacy: it would run any arm slice)"
        elif record[:8] != SLICE_MAGIC or record[8:9] != b"\1":
            why = why or "a hao stub whose __hao_slice record is malformed"
        else:
            arm = arm_slice(sl)
            try:
                chk = hao_codesig.check(bytes(arm)) if arm is not None else {"verdict": "missing"}
            except ValueError as x:
                chk = {"verdict": str(x)}
            if chk["verdict"] != "valid":
                why = why or "a hao stub whose arm slice is %s" % ("not signed validly" if arm is not None else "missing")
            elif record[9] != chk["hash_type"] or record[12:32] != chk["cdhash"]:
                why = why or "a hao stub whose __hao_slice record names another arm slice"
    return (why or "") if found else None


MACHO_MAGICS = (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf", b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe")


def legacy_stubs(system, cli_ok=None):
    """[(path under SYSTEM, why)] for every Mach-O in SYSTEM (fat or thin) with a hao stub that does not name the
    file's arm slice (stub_binding; CLI_OK(path, data) is the launcher exemption), and for every file or
    directory the scan cannot read: it cannot say there is no stub there. Symbolic links are not followed; each
    inode is read once."""
    seen, out = set(), []

    def unreadable(x):
        out.append((os.path.relpath(x.filename or system, system), "not readable (%s): not checked" % x.strerror))
    for base, dirs, names in os.walk(system, onerror=unreadable):
        dirs[:] = [d for d in dirs if not os.path.islink(os.path.join(base, d))]
        for name in names:
            p = os.path.join(base, name)
            rel = os.path.relpath(p, system)
            try:
                st = os.lstat(p)
                if not stat.S_ISREG(st.st_mode) or st.st_size < 4096 or (st.st_dev, st.st_ino) in seen:
                    continue
                seen.add((st.st_dev, st.st_ino))
                with open(p, "rb") as f:
                    if f.read(4) not in MACHO_MAGICS:
                        continue
                    f.seek(0)
                    data = f.read()
            except OSError as x:
                out.append((rel, "not readable (%s): not checked" % x.strerror))
                continue
            why = stub_binding(data, (lambda d, r=rel: cli_ok(r, d)) if cli_ok else None)
            if why:
                out.append((rel, why))
    return sorted(out)


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


def ensure_aot_dirs(system, errors, keep=()):
    """Create or fix the root-owned AOT cache directories, emptied of everything but KEEP (the names of the
    payload's caches just installed into usr/lib/hao/aot); returns a record."""
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
        try:
            dfd = os.open(d, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        except OSError as x:
            errors.append("%s (%s)" % (rel, x.strerror))
            continue
        removed = 0
        try:
            # What is in the directory already is not adopted: hao trusts a cache there because root owns it, so
            # a file someone else left there must not become root's. Every entry but this payload's own caches
            # (KEEP) is removed; hao --aot makes the caches again.
            for name in os.listdir(dfd):
                if rel == AOT_DIRS[0] and name in keep:
                    continue
                st = os.lstat(name, dir_fd=dfd)
                if stat.S_ISDIR(st.st_mode):
                    errors.append(os.path.join(rel, name) + " (a directory: not removed)")
                    continue
                os.unlink(name, dir_fd=dfd)
                removed += 1
            os.fchmod(dfd, 0o755)
            if root:
                os.fchown(dfd, 0, 0)
        finally:
            os.close(dfd)
        if removed:
            done.append("%s (%d earlier files removed)" % (rel, removed))
        st = os.stat(d)
        done.append("%s (uid %d, mode %o)" % (rel, st.st_uid, st.st_mode & 0o7777))
    return done


def macho_uuid(path, mo=None):
    """LC_UUID (hex) of a thin 64-bit Mach-O (the file PATH, or its bytes MO), or None."""
    try:
        if mo is None:
            mo = open(path, "rb").read()
        if mo[:4] != b"\xcf\xfa\xed\xfe":
            return None
        for cmd, off, size in hao_fat.load_commands(mo, path or "Mach-O"):
            if cmd == 0x1B:   # LC_UUID
                return mo[off + 8:off + 24].hex() if size >= 24 else None
    except (OSError, ValueError):
        pass
    return None


# include/hao/cpufeat.h HAO_CPU_BASELINE: aes, pclmul, avx, f16c, rdrand (every supported Mac)
AOT_CPU_BASELINE = 0x1f


def check_aot_caches(system, errors, gen=None, remove=True):
    """The caches in usr/lib/hao/aot (include/hao/aot.h: magic, version 8, the
    guest UUID at 16, the generating libhao at 32, the kind of code identity at
    96 and the CPU profile at 136, in a 144-byte header): keep those of the
    installed libhao that are bound to a signed guest's cdhash (kind 1; hao
    serves no other kind from a system directory) and need no CPU feature
    beyond the baseline (an image's caches must load on every Mac it boots),
    remove the rest (only list them unless REMOVE); returns a record. GEN is the libhao UUID the caches must
    name (default: the installed usr/lib/hao/libhao.dylib's)."""
    d = os.path.join(system, AOT_DIRS[0])
    if gen is None:
        gen = macho_uuid(os.path.join(system, "usr/lib/hao/libhao.dylib"))
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
            h = fh.read(144)
        if len(h) == 144 and h[:8] == b"HAOAOT2\0" and int.from_bytes(h[8:12], "little") == 8 and \
                int.from_bytes(h[12:16], "little") == 144 and int.from_bytes(h[96:100], "little") == 1 and \
                int.from_bytes(h[136:144], "little") & ~AOT_CPU_BASELINE == 0 and \
                h[16:32].hex() == name[:32] and gen is not None and h[32:48].hex() == gen:
            rec["caches"] += 1
            rec["bytes"] += os.path.getsize(f)
        else:
            if remove:
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


def check_names(names):
    """Unsafe if two of NAMES (paths under SYSTEM) are one file on a case-insensitive volume (case or Unicode
    normalisation only) or one names a directory of another (tools/hao_release.py's rule)."""
    names = list(names)
    folded = {}
    for n in names:
        k = unicodedata.normalize("NFD", n).casefold()
        if k in folded:
            raise Unsafe("payload root/%s and root/%s are one file on a case-insensitive volume" % (folded[k], n))
        folded[k] = n
    for n in names:
        d = posixpath.dirname(n)
        while d:
            if unicodedata.normalize("NFD", d).casefold() in folded:
                raise Unsafe("payload root/%s is a file and a directory of root/%s" % (folded[unicodedata.normalize("NFD", d).casefold()], n))
            d = posixpath.dirname(d)


def hash_rest(payload, admitted, staged):
    """Check the sha256 of every admitted payload file not staged (read once, no link followed): the install
    reads none of them, but the payload is what its manifest says or the install is refused."""
    for rel in sorted(set(admitted) - set(staged)):
        try:
            fd = os.open(inside(payload, rel), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except OSError as x:
            raise Unsafe("payload %s: not read (%s)" % (rel, x.strerror))
        h = hashlib.sha256()
        with os.fdopen(fd, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
        if h.hexdigest() != admitted[rel][0]:
            raise Unsafe("payload %s does not match its sha256 in %s" % (rel, MANIFEST))


def open_target(system, rel, write=True):
    """REL under SYSTEM opened for reading and writing in place (only reading unless WRITE), through directory
    descriptors that follow no link; None if it is missing, a link or not a regular file."""
    parts = relative(rel)
    top = os.open(system, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        dfd = dir_at(top, parts[:-1])
    except (OSError, Unsafe):
        return None
    finally:
        os.close(top)
    try:
        fd = os.open(parts[-1], (os.O_RDWR if write else os.O_RDONLY) | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=dfd)
    except OSError:
        return None
    finally:
        os.close(dfd)
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        return None
    return os.fdopen(fd, "r+b" if write else "rb")


def close_all(files):
    while files:
        f = files.pop()
        if f is not None:
            f.close()


def rewrite(f, data):
    """DATA as the whole content of the open file F (same inode: every hard link to an embedded program is
    embedded too, as the image builder may have kept or broken the link)."""
    f.seek(0)
    f.write(data)
    f.truncate()
    f.flush()
    os.fsync(f.fileno())


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
    admitted = payload_inventory(payload)
    work = Staging()
    try:
        return install_staged(system, payload, admitted, work)
    finally:
        work.close()


def load_payload(system, payload, admitted, staged):
    """Steps 0 and the checks before the first write: stage the manifests and every file the install reads, check
    the names they give and the payload's other files; (embed manifest, replace manifest or None, record path)."""
    names = ["embed/manifest.json"] + (["replace/manifest.json"] if "replace/manifest.json" in admitted else [])
    stage(payload, admitted, names, staged)
    try:
        manifest = json.loads(staged.read("embed/manifest.json"))
        replace = json.loads(staged.read("replace/manifest.json")) if len(names) > 1 else None
    except ValueError as x:
        raise Unsafe("a payload manifest is not JSON (%s)" % x)
    manifest_names(manifest, replace)
    record_path = inside(system, "usr/lib/hao/install.json")
    rels = [r for r in admitted if r.startswith("root/")]
    rels += [posixpath.join("embed", e["stub"]) for e in manifest["files"]]
    rels += [posixpath.join("replace", e["file"]) for e in (replace["files"] if replace is not None else [])]
    stage(payload, admitted, rels, staged)
    check_names(r[len("root/"):] for r in staged if r.startswith("root/"))
    hash_rest(payload, admitted, staged)
    return manifest, replace, record_path


def embed_programs(system, manifest, staged, targets, errors, write=True):
    """Step 2 for every entry of the embed MANIFEST: a record of what was embedded (done, already, native,
    missing, bound, restubbed, kept_stubs); errors go to ERRORS. TARGETS holds the file last opened. Without
    WRITE nothing is written: the record says what would be."""
    done, already, native, missing, bound, restubbed, kept_stubs = [], [], [], [], [], [], []
    for e in manifest["files"]:
        if e["path"] in done or e["path"] in missing:
            continue  # listed twice (payloads built before hao-embed skipped repeats)
        try:
            target = inside(system, e["path"], final_link=True)
        except Unsafe as x:
            errors.append("%s (%s; not embedded)" % (e["path"], x))
            continue
        close_all(targets)
        f = open_target(system, e["path"], write)
        if f is None:
            missing.append(e["path"])
            continue
        data = f.read()
        targets.append(f)
        translate = e.get("mode") == "translate"
        stub = staged.read(posixpath.join("embed", e["stub"]))
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
        merged = None
        if digest == e["source_sha256"]:
            merged = hao_fat.combine(data, stub, drop_x86_64=translate)
        else:
            old = stub_binding(data)
            if old is not None:   # a hao stub of another build: this payload's, if that gives the recorded result
                try:
                    merged = hao_fat.combine(data, stub, drop_x86_64=True)
                except ValueError:
                    merged = b""
                if hashlib.sha256(merged).hexdigest() != e["result_sha256"]:
                    if old:
                        errors.append("%s (%s; not replaced: the payload's stub is for another file)" % (e["path"], old))
                    else:
                        kept_stubs.append(e["path"])
                    continue
                restubbed.append(e["path"])
            elif translate:
                errors.append(e["path"] + " (source hash differs; not translated)")
                continue
            else:
                (native if hao_fat.has_cpu(data, hao_fat.CPU_X86_64) else errors).append(e["path"])
                continue
        if hashlib.sha256(merged).hexdigest() != e["result_sha256"]:
            errors.append(e["path"] + " (merge result differs)")
            continue
        why = stub_binding(merged)
        if why:
            errors.append("%s (the payload's stub is %s; not embedded)" % (e["path"], why))
            continue
        if write:
            rewrite(f, merged)
        done.append(e["path"])
    return {"done": done, "already": already, "native": native, "missing": missing, "bound": bound,
            "restubbed": restubbed, "kept_stubs": kept_stubs}


def replace_files(system, replace, staged, targets, errors, write=True, changed=None):
    """Step 3: the paths replaced (or already replaced); errors go to ERRORS, the paths written (or, without
    WRITE, that would be) to CHANGED. Without WRITE nothing is written."""
    replaced = []
    for e in (replace["files"] if replace is not None else []):
        try:
            target = inside(system, e["path"])
        except Unsafe as x:
            errors.append("%s (%s; not replaced)" % (e["path"], x))
            continue
        close_all(targets)
        f = open_target(system, e["path"], write)
        targets.append(f)
        data = f.read() if f is not None else b""
        new = staged.read(posixpath.join("replace", e["file"]))
        if hashlib.sha256(new).hexdigest() != e["result_sha256"]:
            errors.append(e["path"] + " (replacement file corrupt)")
        elif hashlib.sha256(data).hexdigest() == e["result_sha256"]:
            replaced.append(e["path"])
        elif hashlib.sha256(data).hexdigest() != e["source_sha256"]:
            errors.append(e["path"] + " (source hash differs; fix not applicable)")
        elif os.fstat(f.fileno()).st_nlink != 1:
            errors.append(e["path"] + " (has another hard link; not replaced)")
        else:
            if write:
                rewrite(f, new)
            if changed is not None:
                changed.append(e["path"])
            replaced.append(e["path"])
    return replaced


def install_record(installed, emb, required, legacy, replaced, aot_dirs, aot_caches, errors, manifest):
    """The install.json record (step 6)."""
    done = emb["done"]
    return {"installed_files": installed, "embedded": len(done), "already_embedded": len(emb["already"]),
            "required": required, "kept_native": emb["native"], "restubbed": emb["restubbed"],
            "kept_stubs": emb["kept_stubs"], "legacy_stubs": [p for p, _ in legacy],
            "bundle_bound": sum(p in done for p in emb["bound"]),
            "missing": emb["missing"], "replaced": replaced, "aot_dirs": aot_dirs, "aot_caches": aot_caches,
            "errors": errors, "stub_sha256": manifest["stub_sha256"]}


def write_record(system, record_path, record):
    """RECORD as install.json in its directory under SYSTEM, replaced by rename (publish)."""
    top = os.open(system, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        dfd = dir_at(top, relative(os.path.relpath(os.path.dirname(record_path), system)))
    finally:
        os.close(top)
    try:
        publish(dfd, "install.json", io.BytesIO((json.dumps(record, indent=1) + "\n").encode()), 0o644)
    finally:
        os.close(dfd)


def install_staged(system, payload, admitted, staged):
    manifest, replace, record_path = load_payload(system, payload, admitted, staged)
    installed = install_root(admitted, staged, system)
    errors = []
    targets = []   # the file opened for embedding or replacing (one at a time)
    emb = embed_programs(system, manifest, staged, targets, errors)
    done, native, missing, bound, restubbed, kept_stubs = (emb[k] for k in (
        "done", "native", "missing", "bound", "restubbed", "kept_stubs"))
    replaced = replace_files(system, replace, staged, targets, errors)
    aot_dirs = ensure_aot_dirs(system, errors, {posixpath.basename(r) for r in staged
                                                if posixpath.dirname(r) == "root/" + AOT_DIRS[0]})
    aot_caches = check_aot_caches(system, errors)
    reported = {x.split(" (")[0] for x in errors}
    cli = admitted.get("root/usr/libexec/hao", ("",))[0]
    legacy = legacy_stubs(system, lambda rel, d: rel == "usr/libexec/hao" and hashlib.sha256(d).hexdigest() == cli)
    errors += ["%s (%s)" % (p, why) for p, why in legacy if p not in reported]
    required = manifest.get("required", LEGACY_REQUIRED)
    for r in required:
        if r not in done:
            errors.append(r + " (required, not embedded)")
    record = install_record(installed, emb, required, legacy, replaced, aot_dirs, aot_caches, errors, manifest)
    close_all(targets)
    write_record(system, record_path, record)
    print(f"hao: installed {installed} runtime files; embedded {len(done)} executables; "
          f"{sum(p in done for p in bound)} bind their bundle's Info.plist; kept {len(native)} native; {len(restubbed)} restubbed; {len(kept_stubs)} kept hao stubs of another build; "
          f"{len(legacy)} unbound hao stubs; {len(missing)} missing; replaced {replaced}; AOT dirs {aot_dirs}; "
          f"AOT caches {aot_caches['caches']} ({aot_caches['bytes'] >> 20} MB, {len(aot_caches['removed_stale'])} stale removed); "
          f"{len(errors)} errors", flush=True)
    for x in errors[:20]:
        print("hao: error:", x, flush=True)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
