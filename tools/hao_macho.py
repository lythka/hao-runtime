"""Mach-O and dyld-cache format readers used by hao packaging.

Contains no contract inference, SDK probing or boundary classification.
"""
import bisect
import mmap
import os
import struct

CPU_X86_64, CPU_ARM64 = 0x01000007, 0x0100000C
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB, LC_ID_DYLIB, LC_UUID = 0x19, 0x2, 0xb, 0xd, 0x1b
LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB = 0xc, 0x80000018, 0x8000001f
LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB, LC_RPATH = 0x20, 0x80000023, 0x8000001c
LC_DYLD_INFO, LC_DYLD_INFO_ONLY = 0x22, 0x80000022
LC_DYLD_EXPORTS_TRIE, LC_DYLD_CHAINED_FIXUPS = 0x80000033, 0x80000034
DYLIB_CMDS = {LC_LOAD_DYLIB: 'load', LC_LOAD_WEAK_DYLIB: 'weak', LC_REEXPORT_DYLIB: 'reexport',
              LC_LAZY_LOAD_DYLIB: 'lazy', LC_LOAD_UPWARD_DYLIB: 'upward'}
SELF, MAIN, FLAT, WEAK = 0, -1, -2, -3
S_ATTR_INSTRUCTIONS = 0x80000400       # S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS (<mach-o/loader.h>)
SECT_INSTR = {}                        # 'SEG,SECT' -> some image marks it as holding instructions


def uleb(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << s
        s += 7
        if c < 0x80: return r, i


def sleb(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << s
        s += 7
        if c < 0x80:
            if c & 0x40: r -= 1 << s
            return r, i


def cstr(b, i):
    j = b.find(b'\0', i)
    return b[i:j].decode('latin1'), j + 1


class Image:
    """one Mach-O image: a thin slice of a file, or an image inside a dyld shared cache"""

    def __init__(self, read, base_off, path, cache=None):
        self.read, self.path, self.cache = read, path, cache
        h = read(base_off, 32)
        magic, self.cputype, _, self.filetype, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', h)
        if magic != 0xfeedfacf: raise ValueError('not a 64-bit Mach-O: ' + path)
        lc = read(base_off + 32, sizeofcmds)
        self.segs, self.sects, self.deps, self.rpaths = [], [], [], []
        self.id = self.trie = self.fixups = self.dyldinfo = self.symtab = self.dysymtab = self.uuid = None
        p = 0
        for _ in range(ncmds):
            cmd, sz = struct.unpack_from('<II', lc, p)
            if cmd == LC_SEGMENT_64:
                name = lc[p + 8:p + 24].rstrip(b'\0').decode()
                vm, vs, fo, fs = struct.unpack_from('<QQQQ', lc, p + 24)
                self.segs.append((name, vm, vs, fo, fs))
                ns = struct.unpack_from('<I', lc, p + 64)[0]
                for k in range(ns):
                    q = p + 72 + 80 * k
                    sn = lc[q:q + 16].rstrip(b'\0').decode(); sg = lc[q + 16:q + 32].rstrip(b'\0').decode()
                    a, s = struct.unpack_from('<QQ', lc, q + 32)
                    fl = struct.unpack_from('<I', lc, q + 64)[0]
                    self.sects.append((a, a + s, sg, sn))
                    k = sg + ',' + sn
                    SECT_INSTR[k] = SECT_INSTR.get(k, False) or bool(fl & S_ATTR_INSTRUCTIONS)
            elif cmd in DYLIB_CMDS or cmd == LC_ID_DYLIB:
                no = struct.unpack_from('<I', lc, p + 8)[0]
                name = lc[p + no:p + sz].split(b'\0')[0].decode()
                if cmd == LC_ID_DYLIB: self.id = name
                else: self.deps.append((DYLIB_CMDS[cmd], name))
            elif cmd == LC_RPATH:
                no = struct.unpack_from('<I', lc, p + 8)[0]
                self.rpaths.append(lc[p + no:p + sz].split(b'\0')[0].decode())
            elif cmd == LC_DYLD_EXPORTS_TRIE:
                self.trie = struct.unpack_from('<II', lc, p + 8)
            elif cmd == LC_DYLD_CHAINED_FIXUPS:
                self.fixups = struct.unpack_from('<II', lc, p + 8)
            elif cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                self.dyldinfo = struct.unpack_from('<10I', lc, p + 8)
                if not self.trie and self.dyldinfo[9]: self.trie = (self.dyldinfo[8], self.dyldinfo[9])
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack_from('<4I', lc, p + 8)
            elif cmd == LC_DYSYMTAB:
                self.dysymtab = struct.unpack_from('<6I', lc, p + 8)
            elif cmd == LC_UUID:
                self.uuid = lc[p + 8:p + 24].hex().upper()
            p += sz
        self.sects.sort()
        self.sect_starts = [s[0] for s in self.sects]
        self.base = next((vm for n, vm, vs, fo, fs in self.segs if n == '__TEXT'), 0)
        self.base_off = base_off
        self._exports = None

    def linkedit(self, off, size):
        """bytes of a __LINKEDIT blob given by file offset (cache: through the vm address)"""
        if not size: return b''
        if self.cache is None:
            return self.read(self.base_off + off, size)
        for n, vm, vs, fo, fs in self.segs:
            if n == '__LINKEDIT' and fo <= off < fo + fs:
                return self.cache.read_vm(vm + off - fo, size)
        return b''

    def section_of(self, addr):
        i = bisect.bisect_right(self.sect_starts, addr) - 1
        if i >= 0 and addr < self.sects[i][1]:
            return self.sects[i][2] + ',' + self.sects[i][3]
        return None

    def exports(self):
        """name -> ('def', section, offset) | ('reexport', ordinal, name) | ('resolver', section, offset);
        offset: from the image's mach header (the trie's address field)"""
        if self._exports is not None: return self._exports
        ex = {}
        if self.trie:
            b = self.linkedit(*self.trie)
            stack = [(0, '')]
            seen = set()
            while stack:
                off, name = stack.pop()
                if off in seen or off >= len(b): continue
                seen.add(off)
                term, i = uleb(b, off)
                nxt = i + term
                if term:
                    flags, i = uleb(b, i)
                    if flags & 0x08:
                        o, i = uleb(b, i)
                        imp, i = cstr(b, i)
                        ex[name] = ('reexport', o, imp or name)
                    else:
                        a, i = uleb(b, i)
                        kind = flags & 3
                        sect = '__ABS,__abs' if kind == 2 else self.section_of(self.base + a)
                        ex[name] = ('resolver' if flags & 0x10 else 'def', sect if kind != 1 else '__DATA,__thread_vars', a)
                if nxt >= len(b): continue
                n = b[nxt]; i = nxt + 1
                for _ in range(n):
                    e, i = cstr(b, i)
                    c, i = uleb(b, i)
                    stack.append((c, name + e))
        self._exports = ex
        return ex

    def imports(self):
        """[(ordinal, name, weak)]"""
        if self.fixups:
            b = self.linkedit(*self.fixups)
            _, _, io, so, ic, ifmt, sfmt = struct.unpack_from('<7I', b, 0)
            out = []
            for k in range(ic):
                if ifmt == 1:
                    v = struct.unpack_from('<I', b, io + 4 * k)[0]; o, w, no = v & 0xff, (v >> 8) & 1, v >> 9
                    if o > 0xf0: o -= 0x100
                elif ifmt == 2:
                    v = struct.unpack_from('<I', b, io + 8 * k)[0]; o, w, no = v & 0xff, (v >> 8) & 1, v >> 9
                    if o > 0xf0: o -= 0x100
                elif ifmt == 3:
                    v = struct.unpack_from('<Q', b, io + 16 * k)[0]; o, w, no = v & 0xffff, (v >> 16) & 1, v >> 32
                    if o > 0xfff0: o -= 0x10000
                else:
                    break
                out.append((o, cstr(b, so + no)[0], bool(w)))
            return out
        if self.dyldinfo and self.cache is None:
            out = set()
            for off, size in ((self.dyldinfo[2], self.dyldinfo[3]), (self.dyldinfo[4], self.dyldinfo[5]), (self.dyldinfo[6], self.dyldinfo[7])):
                out |= self._bind_opcodes(self.linkedit(off, size))
            return sorted(out)
        if self.symtab and self.dysymtab:
            symoff, nsyms, stroff, strsize = self.symtab
            iu, nu = self.dysymtab[4], self.dysymtab[5]
            if not nu: return []
            st = self.linkedit(symoff + 16 * iu, 16 * nu)
            strs = self.linkedit(stroff, strsize)
            out = []
            for k in range(nu):
                strx, ntype, nsect, desc, val = struct.unpack_from('<IBBhQ', st, 16 * k)
                o = (desc >> 8) & 0xff
                if o >= 0xfe: o -= 0x100
                elif o == 0xfd: o = WEAK
                out.append((o, cstr(strs, strx)[0], bool(desc & 0x40)))
            return out
        return []

    @staticmethod
    def _bind_opcodes(b):
        out, i, o, name, weak = set(), 0, 0, '', False
        while i < len(b):
            c = b[i]; i += 1
            op, imm = c & 0xf0, c & 0x0f
            if op == 0x00: continue
            elif op == 0x10: o = imm
            elif op == 0x20: o, i = uleb(b, i)
            elif op == 0x30: o = imm - 16 if imm else 0
            elif op == 0x40: name, i = cstr(b, i); weak = bool(imm & 1)
            elif op == 0x50: pass
            elif op == 0x60: _, i = sleb(b, i)
            elif op == 0x70: _, i = uleb(b, i)
            elif op == 0x80: _, i = uleb(b, i)
            elif op == 0x90: out.add((o, name, weak))
            elif op == 0xa0: out.add((o, name, weak)); _, i = uleb(b, i)
            elif op == 0xb0: out.add((o, name, weak))
            elif op == 0xc0: out.add((o, name, weak)); _, i = uleb(b, i); _, i = uleb(b, i)
            elif op == 0xd0:
                if imm == 0: _, i = uleb(b, i)
            else: break
        return out


class Cache:
    """a dyld shared cache: main file plus subcaches, images by install name"""

    def __init__(self, path):
        self.path = path
        self.files, self.maps = [], []         # maps: (addr, size, file index, fileoff)
        self._add(path, True)
        self.maps.sort()
        self.starts = [m[0] for m in self.maps]
        self.images = {}
        self._img = {}

    def _add(self, path, main):
        f = open(path, 'rb')
        mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
        fi = len(self.files)
        self.files.append(mm)
        h = mm[:0x200]
        if not h.startswith(b'dyld_v1'): raise ValueError('not a dyld shared cache: ' + path)
        moff, mcount = struct.unpack_from('<II', h, 0x10)
        for k in range(mcount):
            a, s, fo = struct.unpack_from('<QQQ', mm, moff + 32 * k)
            self.maps.append((a, s, fi, fo))
        if not main: return
        self.uuid = h[0x58:0x68].hex().upper()
        sco, scn = struct.unpack_from('<II', h, 0x188)
        v2 = moff > 0x1c8
        for k in range(scn):
            if v2:
                suffix = mm[sco + 56 * k + 24: sco + 56 * k + 56].split(b'\0')[0].decode()
            else:
                suffix = '.%d' % (k + 1)
            if os.path.exists(path + suffix): self._add(path + suffix, False)
        io, ic = struct.unpack_from('<II', h, 0x1c0)
        if not ic: io, ic = struct.unpack_from('<II', h, 0x18)
        self.paths = []
        for k in range(ic):
            a, _, _, po = struct.unpack_from('<QQQI', mm, io + 32 * k)
            p = mm[po:mm.find(b'\0', po)].decode()
            self.paths.append((p, a))
        self.addr_of = dict(self.paths)

    def read_vm(self, a, n):
        i = bisect.bisect_right(self.starts, a) - 1
        if i < 0: return b''
        ma, ms, fi, fo = self.maps[i]
        if a + n > ma + ms: n = max(0, ma + ms - a)
        o = fo + a - ma
        return self.files[fi][o:o + n]

    def image(self, path):
        if path in self._img: return self._img[path]
        a = self.addr_of.get(path)
        img = None
        if a is not None:
            img = Image(lambda off, n: self.read_vm(off, n), a, path, self)
        self._img[path] = img
        return img

    def ids(self):
        """install name (LC_ID_DYLIB) and path -> path, for every image"""
        if self.images: return self.images
        for p, a in self.paths:
            self.images[p] = p
        for p, a in self.paths:
            try:
                im = self.image(p)
                if im and im.id: self.images.setdefault(im.id, p)
            except (ValueError, struct.error):
                pass
        return self.images


def file_slices(path):
    try:
        with open(path, 'rb') as f:
            h = f.read(4096)
    except OSError:
        return []
    if len(h) < 8: return []
    magic = struct.unpack_from('>I', h)[0]
    out = []
    if magic in (0xcafebabe, 0xcafebabf):
        n = struct.unpack_from('>I', h, 4)[0]
        for k in range(min(n, 40)):
            if magic == 0xcafebabe:
                ct, cs, off, size, _ = struct.unpack_from('>iiIII', h, 8 + 20 * k)
            else:
                ct, cs, off, size, _, _ = struct.unpack_from('>iiQQII', h, 8 + 32 * k)
            out.append((ct & 0xffffffff, cs & 0xff, off))
    elif struct.unpack_from('<I', h)[0] == 0xfeedfacf:
        ct, cs = struct.unpack_from('<ii', h, 4)
        out.append((ct & 0xffffffff, cs & 0xff, 0))
    return out


class DiskFile:
    def __init__(self, path):
        f = open(path, 'rb')
        self.mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)

    def read(self, off, n):
        return self.mm[off:off + n]


