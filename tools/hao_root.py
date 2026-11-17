"""hao_root: a release's System root with its cryptexes mounted on their own.

On an installed system the OS and App cryptexes are disk images mounted at
System/Volumes/Preboot/Cryptexes/NAME (System/Cryptexes/NAME links there). An IPSW ships each as its own image, so
a machine that mounts the images read-only has them in separate folders, not under the system image's mount. Root
puts them back where the system has them: every path under ROOT/System/Volumes/Preboot/Cryptexes/NAME is read from
the folder given for NAME, and every recorded path is the one a tree with the cryptexes in place would give, so the
packaging tools' outputs do not depend on how the images are mounted. With no cryptex folder given, nothing is
mapped and every path is the host's (os.path.realpath).
"""
import os
import re

CRYPTEXES = "System/Volumes/Preboot/Cryptexes"


def parse(specs):
    """{NAME: folder} from ['NAME=DIR', ...] (--cryptex); ValueError for a bad one."""
    out = {}
    for s in specs or ():
        name, sep, d = s.partition("=")
        if not sep or not re.fullmatch(r"[A-Za-z][A-Za-z0-9]{0,31}", name) or not d:
            raise ValueError("--cryptex %r: give NAME=DIR (e.g. OS=/Volumes/...)" % s)
        if name in out:
            raise ValueError("--cryptex %s given twice" % name)
        if not os.path.isdir(d):
            raise ValueError("--cryptex %s: no folder %s" % (name, d))
        out[name] = d
    return out


class Root:
    """ROOT (any spelling) with the cryptex folders CRYPTEXES {NAME: DIR} at their mount points. Paths "in the tree"
    are host paths under ROOT's real path; host() gives the file a tree path reads."""

    def __init__(self, root, cryptexes=None):
        self.root = os.path.abspath(root)
        self.real = os.path.realpath(root)
        self.mounts = sorted((CRYPTEXES + "/" + n, os.path.realpath(d)) for n, d in (cryptexes or {}).items())

    def _rel(self, p):
        """P relative to ROOT (either spelling), or None if it is not under it."""
        for base in (self.real, self.root):
            if p == base:
                return ""
            if p.startswith(base.rstrip("/") + "/"):
                return p[len(base.rstrip("/")) + 1:]
        return None

    def host(self, p):
        """The host path of P, a path in the tree: the cryptex folder's file under a mount point, else P."""
        if not self.mounts:
            return p
        r = self._rel(os.path.normpath(p))
        for point, d in self.mounts:
            if r is not None and (r == point or r.startswith(point + "/")):
                return d + r[len(point):]
        return p

    def tree(self, p):
        """P (a host path) as a path in the tree: a file of a cryptex folder under its mount point, else P."""
        for point, d in self.mounts:
            if p == d or p.startswith(d + "/"):
                return os.path.join(self.real, point) + p[len(d):]
        return p

    def resolve(self, p):
        """P with every symbolic link followed as in the tree (os.path.realpath's rules: a missing name stays as
        written), as a path in the tree."""
        if not self.mounts:
            return os.path.realpath(p)
        todo, done, hops = self.tree(os.path.abspath(p)).split("/"), "/", 0
        while todo:
            c = todo.pop(0)
            if c in ("", "."):
                continue
            if c == "..":
                done = os.path.dirname(done)
                continue
            nxt = os.path.join(done, c)
            h = self.host(nxt)
            if hops <= 40 and os.path.islink(h):
                hops += 1
                t = os.readlink(h)
                todo = t.split("/") + todo
                if t.startswith("/"):
                    done = "/"
                continue
            done = nxt
        return done

    def file(self, p):
        """The host file P (a path in the tree, links and all) is."""
        return self.host(self.resolve(p))

    def relpath(self, p):
        """P (a path in the tree or a host path) resolved, relative to ROOT (starts with .. if outside it)."""
        return os.path.relpath(self.resolve(p), self.real)

    def walk(self):
        """os.walk of the whole tree as host folders: ROOT (less what is under a mount point), then each cryptex
        folder. A caller that prunes a yielded dirs list prunes the walk, as with os.walk."""
        points = {p for p, _ in self.mounts}
        for base, dirs, files in os.walk(self.root):
            r = self._rel(base)
            if points and r is not None:
                dirs[:] = [d for d in dirs if (r + "/" + d if r else d) not in points]
            yield base, dirs, files
        for _, d in self.mounts:
            yield from os.walk(d)

    def key(self, p):
        """P (a host path a walk gave) relative to ROOT in the tree: sorting by it orders as a walk of the tree."""
        r = self._rel(self.tree(p))
        return p if r is None else r
