#!/usr/bin/env python3
"""Interface units for hao's typed boundary (include/hao/bridge.h).

A unit describes the exports of one x86_64 native library, keyed by its LC_UUID,
and installs as /usr/lib/hao/if/<uuid> (32 lowercase hex digits). This is the
format's reference writer and checker; src/ifunit.c is the runtime parser, and
both enforce the same rules.

  hao_ifunit.py write OUT (--uuid HEX | --lib MACHO) [RECORDS.tsv]   (stdin without a file)
  hao_ifunit.py dump UNIT
  hao_ifunit.py check UNIT [--uuid HEX]
  hao_ifunit.py uuid MACHO          the x86_64 slice's LC_UUID (32 hex digits)

RECORDS.tsv, one exported symbol per line ('#' comments; '-' for an empty field):
  name     Mach-O symbol name with its leading '_'
  class    EXACT | TYPED | REFUSED | UNKNOWN
  kind     - | B c C s S (narrow C result) | b h H t T (Swift async narrow result)
           | A (call through abi.c with enc)
           | K (Swift packed stack arguments, tools/gen_stackpack.py: class TYPED, enc lists for SysV
             slots 2, 3, ... the arm64 byte offset and width of the argument each carries,
             "OFF.W,OFF.W,...", W in 1 2 4 8, at most 16, inside the 16 guest stack words hao copies)
           | Z (std::string parameters converted, src/cxxstr.c: enc is 'P' or '.' then one role
             per register x0..x5, '-' r v w; tools/gen_cxxstr.py)
  argmask  - (the full argument scan) or a hex mask of the slots that may carry
           guest code: bits 0-7 x0-x7, bits 8-15 stack argument slots 0-7
  resmask  - or a hex mask: 1 the result may be a native function pointer, 2 a block
  enc      arm64 ObjC type encoding (required with kind A)
  why      reason (required for REFUSED)
  thunk    (optional 8th field) the generated thunk pair's key: the call goes through
           _hao_gh_KEY / _hao_nh_KEY of a thunk library (tools/gen_bridge.py); class TYPED,
           kind - or A (A: enc is the abi.c equivalent, used with HAO_NO_THUNKS=1)
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hao_fat  # noqa: E402

MAGIC = b'HAOIF01\0'
VERSION = 2
HDR = struct.Struct('<8sII16sIIIIQQ')     # 64 bytes
REC = struct.Struct('<IIIIHBBBBH')       # 24 bytes
CLASSES = {'UNKNOWN': 0, 'EXACT': 1, 'TYPED': 2, 'REFUSED': 3}
NARROW = set('BcCsSbhHtT')
STACKPACK = 'K'
MAX_PACK = 32


def stackpack(enc):
    """[(arm64 offset, width)] of a kind K encoding, or raise Bad (src/ifunit.c hao_ifunit_stackpack)"""
    out, end = [], 0
    parts = enc.split(',') if enc else []
    if not parts or len(parts) > MAX_PACK: raise Bad('stack pack of %d slots' % len(parts))
    for p in parts:
        o, _, w = p.partition('.')
        if not (o.isdigit() and w in ('1', '2', '4', '8')) or (len(o) > 1 and o[0] == '0'): raise Bad('stack pack %r' % enc)
        o, w = int(o), int(w)
        if o % w or o < end or o + w > 8 * MAX_PACK: raise Bad('stack pack %r: offset %d' % (enc, o))
        out.append((o, w)); end = o + w
    return out
F_MASKED, F_SIG, F_THUNK, F_PROVEN = 1, 2, 4, 8
# only the current scalar proof: earlier ones (scalar-leaf-v1, scalar-call-v1/v2) accepted arguments used as memory
PROOF_PREFIX = 'source=code: proof=scalar-call-v3;'
RESULT_PROOF_PREFIX = 'source=code: proof=result-only-v1;'
# Result-only records are TYPED C narrow conversions, with no mask/encoding.
# They preserve the fallback argument scan and claim nothing about arguments.
PROOF_PREFIXES = (PROOF_PREFIX, RESULT_PROOF_PREFIX)
MAX_KEY = 64
KEYCHARS = set('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_')
MAX_FILE = 64 << 20
MAX_RECS = 1 << 20
M = (1 << 64) - 1


def hash64(b, seed=0):
    """hao_aot_hash64 (src/aot_policy.c)"""
    n = len(b)
    h = (seed ^ (0x9E3779B97F4A7C15 * (n + 1))) & M
    i = 0
    while n - i >= 8:
        w = int.from_bytes(b[i:i + 8], 'little')
        h = ((h ^ w) * 0xff51afd7ed558ccd) & M
        h ^= h >> 29
        i += 8
    w = int.from_bytes(b[i:] + bytes(8 - (n - i)), 'little')
    h = ((h ^ w ^ (((n - i) << 56) & M)) * 0xc4ceb9fe1a85ec53) & M
    return h ^ (h >> 32)


class Bad(Exception):
    pass


def macho_uuid(path):
    """LC_UUID of the x86_64 slice (or of a thin x86_64 file)"""
    return macho_uuid_of(open(path, 'rb').read(), path)


def macho_uuid_of(data, what='Mach-O'):
    """macho_uuid for a file's bytes; Bad for anything that is not a well-formed x86_64 Mach-O with an LC_UUID"""
    sl = hao_fat.slices(data)
    if sl is None:
        raise Bad('%s: not a Mach-O' % what)
    mo = next((p for cpu, _, p in sl if cpu == hao_fat.CPU_X86_64), None)
    if mo is None:
        raise Bad('%s: %s' % (what, 'no x86_64 slice' if bytes(data[:3]) == b'\xca\xfe\xba' else 'not an x86_64 Mach-O'))
    if len(mo) < 32 or struct.unpack_from('<I', mo)[0] != hao_fat.MH_MAGIC_64:
        raise Bad('%s: not an x86_64 Mach-O' % what)
    try:
        for cmd, off, size in hao_fat.load_commands(mo, what):
            if cmd == 0x1b:
                if size < 24:
                    raise Bad('%s: short LC_UUID' % what)
                return bytes(mo[off + 8:off + 24]).hex()
    except ValueError as e:
        raise Bad(str(e))
    raise Bad('%s: no LC_UUID' % what)


def cxxstr_enc_ok(e):
    """hao_cxxstr_enc_ok (include/hao/bridge.h)"""
    if not e or e[0] not in 'P.': return False
    r = e[1:]
    return 1 <= len(r) <= 6 and not set(r) - set('-rvw') and any(c != '-' for c in r)


def check_rec(name, cls, kind, flags, enc, why, thunk='', *, allow_legacy_code=False):
    sig, thk = flags & F_SIG, flags & F_THUNK
    code, proven = why.startswith('source=code'), bool(flags & F_PROVEN)
    if (code or proven) and not (allow_legacy_code and code and not proven):
        scalar = why.startswith(PROOF_PREFIX) and flags & F_MASKED and cls in (1, 2)
        result = (why.startswith(RESULT_PROOF_PREFIX) and not flags & F_MASKED and
                  cls == 2 and kind and chr(kind) in 'BcCsS' and not enc)
        if not (code and proven and (scalar or result) and
                not sig and not thk and (not kind or chr(kind) in 'BcCsS')):
            raise Bad('%s: stale or invalid code proof; regenerate interface units' % name)
    if kind and chr(kind) == STACKPACK:
        if cls != 2 or sig or thk or proven or code:
            raise Bad('%s: a stack pack is TYPED with no other conversion' % name)
        stackpack(enc)
    elif kind and chr(kind) not in NARROW and chr(kind) != 'Z':
        raise Bad('%s: result kind %r' % (name, chr(kind)))
    if kind and chr(kind) == 'Z' and (sig or thk or not cxxstr_enc_ok(enc)):
        raise Bad('%s: a std::string conversion (kind Z) needs its roles in enc' % name)
    if sig and (not enc or kind):
        raise Bad('%s: a signature call needs an encoding and no kind' % name)
    if bool(thk) != bool(thunk):
        raise Bad('%s: a thunk key needs the thunk flag and the flag a key' % name)
    if thunk and (len(thunk) > MAX_KEY or set(thunk) - KEYCHARS or kind):
        raise Bad('%s: thunk key %r' % (name, thunk))
    if any(c < 0x21 or c > 0x7e for c in enc.encode()):
        raise Bad('%s: encoding byte out of range' % name)
    if cls == 1 and (sig or thk or kind):
        raise Bad('%s: EXACT with a conversion' % name)
    if cls == 2 and not (sig or thk or kind):
        raise Bad('%s: TYPED without a conversion' % name)
    if cls == 3 and (not why or sig or thk or kind or flags & F_MASKED):
        raise Bad('%s: REFUSED needs a reason and no conversion' % name)
    if cls == 0 and (sig or thk or kind or flags & F_MASKED):
        raise Bad('%s: UNKNOWN with a type' % name)


def parse_records(lines):
    recs = {}
    for ln, line in enumerate(lines, 1):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) not in (7, 8):
            raise Bad('line %d: %d fields, expected 7 or 8' % (ln, len(f)))
        if len(f) == 7: f.append('-')
        name, cls, kind, argmask, resmask, enc, why, thunk = [x if x != '-' else '' for x in f]
        if not name:
            raise Bad('line %d: no name' % ln)
        if cls not in CLASSES:
            raise Bad('line %d: class %r' % (ln, cls))
        flags = 0
        if kind == 'A':
            flags |= F_SIG
            kind = ''
        if len(kind) > 1:
            raise Bad('line %d: kind %r' % (ln, kind))
        am = 0
        if argmask:
            am = int(argmask, 16)
            if am >> 16:
                raise Bad('line %d: argmask %s' % (ln, argmask))
            flags |= F_MASKED
        rm = int(resmask, 16) if resmask else 0
        if rm & ~3:
            raise Bad('line %d: resmask %s' % (ln, resmask))
        if thunk: flags |= F_THUNK
        if why.startswith(PROOF_PREFIXES): flags |= F_PROVEN
        if flags & F_PROVEN and (am & ~0x3f or rm): raise Bad('%s: non-scalar code proof mask' % name)
        if why.startswith(RESULT_PROOF_PREFIX) and am: raise Bad('%s: result-only proof has an argument mask' % name)
        k = ord(kind) if kind else 0
        check_rec(name, CLASSES[cls], k, flags, enc, why, thunk)
        if name in recs:
            raise Bad('line %d: %s listed twice' % (ln, name))
        recs[name] = (CLASSES[cls], k, flags, am, rm, enc, why, thunk)
    return recs


def build(uuid_hex, recs):
    uuid = bytes.fromhex(uuid_hex)
    if len(uuid) != 16:
        raise Bad('UUID %s is not 16 bytes' % uuid_hex)
    names = sorted(recs, key=lambda s: s.encode())
    strtab = bytearray(b'\0')
    index = {}

    def s(x):
        if not x:
            return 0
        if x not in index:
            index[x] = len(strtab)
            strtab.extend(x.encode() + b'\0')
        return index[x]
    body = bytearray()
    for n in names:
        cls, kind, flags, am, rm, enc, why, thunk = recs[n]
        body += REC.pack(s(n), s(enc), s(why), s(thunk), am, cls, kind, flags, rm, 0)
    if len(names) > MAX_RECS:
        raise Bad('%d records' % len(names))
    payload = bytes(body) + bytes(strtab)
    size = HDR.size + len(payload)
    if size > MAX_FILE:
        raise Bad('unit of %d bytes' % size)
    hdr = HDR.pack(MAGIC, VERSION, HDR.size, uuid, len(names), REC.size, HDR.size + len(body), len(strtab), size,
                   hash64(payload))
    return hdr + payload


def read(data, want_uuid=None, *, allow_legacy_code=False):
    """validate like hao_ifunit_check; returns (uuid_hex, [(name, cls, kind, flags, argmask, resmask, enc, why, thunk)])"""
    if len(data) < HDR.size:
        raise Bad('truncated')
    magic, ver, hsz, uuid, n, rsz, so, ss, fsz, ph = HDR.unpack_from(data)
    if magic != MAGIC: raise Bad('not a hao interface unit')
    if ver != VERSION: raise Bad('format version %d' % ver)
    if hsz != HDR.size or rsz != REC.size: raise Bad('header or record size')
    if fsz != len(data) or fsz > MAX_FILE: raise Bad('file size')
    if want_uuid and uuid.hex() != want_uuid.lower(): raise Bad('library UUID differs')
    if n > MAX_RECS or so != HDR.size + n * REC.size or ss < 1 or so + ss != len(data): raise Bad('layout')
    if hash64(data[HDR.size:]) != ph: raise Bad('payload hash mismatch')
    st = data[so:]
    if st[0] or st[-1]: raise Bad('string table not NUL-delimited')

    def gs(o):
        try:
            return st[o:st.index(b'\0', o)].decode() if o else ''
        except UnicodeDecodeError:
            raise Bad('string at %d is not UTF-8' % o)
    out, prev = [], None
    for k in range(n):
        no, eo, wo, to, am, cls, kind, flags, rm, rsv = REC.unpack_from(data, HDR.size + k * REC.size)
        if not no or no >= ss or not st[no]: raise Bad('record %d: no name' % k)
        if eo >= ss or wo >= ss or to >= ss: raise Bad('record %d: string offset' % k)
        name = gs(no)
        if prev is not None and prev.encode() >= name.encode(): raise Bad('record %d: names not strictly increasing' % k)
        prev = name
        if cls > 3 or flags & ~15 or rm & ~3 or rsv: raise Bad('record %d (%s): field out of range' % (k, name))
        if flags & F_PROVEN and (am & ~0x3f or rm): raise Bad('%s: non-scalar code proof mask' % name)
        if gs(wo).startswith(RESULT_PROOF_PREFIX) and am: raise Bad('%s: result-only proof has an argument mask' % name)
        check_rec(name, cls, kind, flags, gs(eo), gs(wo), gs(to), allow_legacy_code=allow_legacy_code)
        out.append((name, cls, kind, flags, am, rm, gs(eo), gs(wo), gs(to)))
    return uuid.hex(), out


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    try:
        if cmd == 'write':
            out, args = argv[2], argv[3:]
            uuid = None
            if args[:1] == ['--uuid']:
                uuid, args = args[1], args[2:]
            elif args[:1] == ['--lib']:
                uuid, args = macho_uuid(args[1]), args[2:]
            else:
                sys.exit('write: --uuid HEX or --lib MACHO')
            lines = open(args[0]).readlines() if args else sys.stdin.readlines()
            data = build(uuid.replace('-', ''), parse_records(lines))
            read(data)
            open(out, 'wb').write(data)
        elif cmd == 'dump':
            uuid, recs = read(open(argv[2], 'rb').read())
            print('# uuid %s, %d records' % (uuid, len(recs)))
            inv = {v: k for k, v in CLASSES.items()}
            for name, cls, kind, flags, am, rm, enc, why, thunk in recs:
                k = 'A' if flags & F_SIG else (chr(kind) if kind else '-')
                print('\t'.join([name, inv[cls], k, '%#x' % am if flags & F_MASKED else '-', '%#x' % rm if rm else '-',
                                 enc or '-', why or '-'] + ([thunk] if thunk else [])))
        elif cmd == 'check':
            want = argv[4] if argv[3:4] == ['--uuid'] else None
            uuid, recs = read(open(argv[2], 'rb').read(), want)
            print('%s: valid, uuid %s, %d records' % (argv[2], uuid, len(recs)))
        elif cmd == 'uuid':
            print(macho_uuid(argv[2]))
        else:
            sys.exit(__doc__)
    except Bad as e:
        sys.exit('hao_ifunit: %s' % e)


if __name__ == '__main__':
    main(sys.argv)
