"""tools/hao-runtime-on-mac pin(): a stub or a fat dyld signed with a larger reservation (as macOS 26's codesign does)
comes back byte for byte as the pinned file; a file no trim can make is left as it is.
  python3 -I tests/tools/onmac_pin_check.py STUB... FAT_DYLD"""
import hashlib, importlib.machinery, importlib.util, os, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, '..', '..', 'tools', 'hao-runtime-on-mac')
loader = importlib.machinery.SourceFileLoader('onmac', tool)
spec = importlib.util.spec_from_loader('onmac', loader)
m = importlib.util.module_from_spec(spec)
loader.exec_module(m)
fat = m.hao_fat


def inflate(mo, extra):
    d = bytearray(mo)
    count, at, sig, le = struct.unpack_from('<I', d, 16)[0], 32, None, None
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', d, at)
        if cmd == 0x1d:
            sig = at
        elif cmd == 0x19 and d[at + 8:at + 24].rstrip(b'\0') == b'__LINKEDIT':
            le = at
        at += size
    off, size = struct.unpack_from('<II', d, sig + 8)
    struct.pack_into('<I', d, sig + 12, size + extra)
    fs = struct.unpack_from('<Q', d, le + 48)[0] + extra
    struct.pack_into('<Q', d, le + 48, fs)
    struct.pack_into('<Q', d, le + 32, (fs + 0x3fff) & ~0x3fff)
    d += bytes(extra)
    return m.trim_signature(bytes(d), size + extra)


bad = 0
*stubs, dyld = sys.argv[1:]
for p in stubs:
    orig = open(p, 'rb').read()
    big = inflate(orig, 18000)
    if big is None or big == orig or m.pin(big, hashlib.sha256(orig).hexdigest()) != orig:
        print('FAIL stub', p); bad += 1
    if m.pin(big, '0' * 64) is not big:
        print('FAIL unpinnable stub changed', p); bad += 1
d = open(dyld, 'rb').read()
x = [s for c, _, s in fat.slices(d) if c == fat.CPU_X86_64][0]
big = fat.replace_slice(d, fat.CPU_X86_64, inflate(x, 18000))
if m.pin(big, hashlib.sha256(d).hexdigest()) != d:
    print('FAIL fat dyld'); bad += 1
print('pin: %d stubs and a fat dyld, %d failed' % (len(stubs), bad))
sys.exit(1 if bad else 0)
