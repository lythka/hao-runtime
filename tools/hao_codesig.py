"""hao_codesig: what a Mach-O slice's embedded code signature says about loading libraries.

Pure Python, like hao_fat.py (the image builder has no developer tools). It
reads the slice's LC_CODE_SIGNATURE SuperBlob: the CodeDirectory's flags,
platform identifier and Team ID, and the XML entitlements. src/libval.c
reads the same fields at run time; the two must agree.

Native AMFI/dyld behaviour the policy mirrors (measured on the macOS 27.2
host):
  - library validation: a platform binary (kernel flag CS_FORCED_LV, e.g.
    /bin/sleep), a hardened-runtime binary (CodeDirectory flag runtime) or
    one signed with the library flag loads only platform libraries or
    libraries of its own Team ID, unless it has
    com.apple.security.cs.disable-library-validation;
  - DYLD_* environment variables: ignored by dyld for platform and
    hardened-runtime binaries (DYLD_INSERT_LIBRARIES into /bin/echo or an
    ad hoc `-o runtime` binary: not loaded; into a plain ad hoc one: loaded),
    unless com.apple.security.cs.allow-dyld-environment-variables.
"""
import hashlib
import plistlib
import struct

import hao_fat

LC_CODE_SIGNATURE = 0x1D
CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0
CSMAGIC_CODEDIRECTORY = 0xFADE0C02
CSMAGIC_EMBEDDED_ENTITLEMENTS = 0xFADE7171
CSSLOT_CODEDIRECTORY, CSSLOT_ENTITLEMENTS = 0, 5
CSSLOT_ALTERNATE_CODEDIRECTORIES = range(0x1000, 0x1005)
# Special slots (hashes of data outside the code pages, at -N before the code
# hashes): -1 the Info.plist (a bundle's Contents/Info.plist, or a tool's
# __TEXT,__info_plist), -3 the resource seal (a bundle's
# _CodeSignature/CodeResources).
CSSLOT_INFOSLOT, CSSLOT_RESOURCEDIR = 1, 3
CS_REQUIRE_LV, CS_RUNTIME = 0x2000, 0x10000
DLV = "com.apple.security.cs.disable-library-validation"
DYLD_ENV = "com.apple.security.cs.allow-dyld-environment-variables"


def signature_range(mo):
    """(offset, size) from the last LC_CODE_SIGNATURE of a thin 64-bit
    Mach-O, None without one; ValueError if the load commands are malformed
    (hao_fat.load_commands) or the command is too short to hold the range."""
    sig = None
    for cmd, off, size in hao_fat.load_commands(mo, "Mach-O"):
        if cmd == LC_CODE_SIGNATURE:
            if size < 16:
                raise ValueError("Mach-O: short LC_CODE_SIGNATURE")
            sig = struct.unpack_from("<II", mo, off + 8)
    return sig


def info(mo):
    """dict(signed, flags, platform, team, entitlements) of a thin 64-bit
    little-endian Mach-O, or None if it is not one. Malformed signatures
    read as unsigned."""
    if len(mo) < 32 or struct.unpack_from("<I", mo)[0] != 0xFEEDFACF:
        return None
    out = {"signed": False, "flags": 0, "platform": 0, "team": None, "entitlements": {}}
    try:
        sig = signature_range(mo)
    except ValueError:
        return out
    if not sig or sig[0] + sig[1] > len(mo) or sig[1] < 12:
        return out
    blob = mo[sig[0]:sig[0] + sig[1]]
    magic, length, count = struct.unpack_from(">III", blob)
    if magic != CSMAGIC_EMBEDDED_SIGNATURE or length > len(blob) or 12 + 8 * count > length:
        return out
    for k in range(count):
        typ, boff = struct.unpack_from(">II", blob, 12 + 8 * k)
        if boff + 8 > length:
            continue
        bmagic, blen = struct.unpack_from(">II", blob, boff)
        if boff + blen > length or blen < 8:
            continue
        b = blob[boff:boff + blen]
        if typ == CSSLOT_CODEDIRECTORY and bmagic == CSMAGIC_CODEDIRECTORY and blen >= 44:
            version, flags = struct.unpack_from(">II", b, 8)
            out["signed"] = True
            out["flags"] = flags
            out["platform"] = b[38]
            if version >= 0x20200 and blen >= 52:
                toff = struct.unpack_from(">I", b, 48)[0]
                if toff and toff < blen:
                    out["team"] = b[toff:].split(b"\0")[0].decode(errors="replace")
        elif typ == CSSLOT_ENTITLEMENTS and bmagic == CSMAGIC_EMBEDDED_ENTITLEMENTS:
            try:
                ents = plistlib.loads(b[8:])
                if isinstance(ents, dict):
                    out["entitlements"] = ents
            except Exception:
                pass
    return out


def library_validated(i):
    """True if the program, run natively, loads only platform or same-team libraries."""
    if not i or not i["signed"] or i["entitlements"].get(DLV) is True:
        return False
    return bool(i["platform"]) or bool(i["flags"] & (CS_RUNTIME | CS_REQUIRE_LV))


def dyld_env_blocked(i):
    """True if dyld ignores DYLD_* environment variables for the program natively."""
    if not i or not i["signed"] or i["entitlements"].get(DYLD_ENV) is True:
        return False
    return bool(i["platform"]) or bool(i["flags"] & CS_RUNTIME)


HASH_SIZES = {1: 20, 2: 32, 3: 20, 4: 48}   # CS_HASHTYPE_SHA1, SHA256, SHA256_TRUNCATED, SHA384


def digest(hash_type, data):
    """The CodeDirectory hash of DATA for HASH_TYPE (cs_blobs.h)."""
    if hash_type == 1:
        return hashlib.sha1(data).digest()
    if hash_type in (2, 3):
        return hashlib.sha256(data).digest()[:HASH_SIZES[hash_type]]
    if hash_type == 4:
        return hashlib.sha384(data).digest()
    raise ValueError("unknown CodeDirectory hash type %d" % hash_type)


def code_directories(mo):
    """[(offset in MO, hashType, hashSize, nSpecialSlots, hashOffset)] of every
    CodeDirectory (primary and alternates) in a thin Mach-O's signature.
    ValueError if the signature is malformed; [] if there is none."""
    if len(mo) < 32 or struct.unpack_from("<I", mo)[0] != 0xFEEDFACF:
        raise ValueError("not a thin 64-bit Mach-O")
    sig = signature_range(mo)
    if not sig:
        return []
    base, size = sig
    if base + size > len(mo) or size < 12:
        raise ValueError("signature outside the file")
    magic, length, count = struct.unpack_from(">III", mo, base)
    if magic != CSMAGIC_EMBEDDED_SIGNATURE or length > size or 12 + 8 * count > length:
        raise ValueError("bad signature SuperBlob")
    out = []
    for k in range(count):
        typ, boff = struct.unpack_from(">II", mo, base + 12 + 8 * k)
        if typ != CSSLOT_CODEDIRECTORY and typ not in CSSLOT_ALTERNATE_CODEDIRECTORIES:
            continue
        if boff + 44 > length:
            raise ValueError("CodeDirectory outside the signature")
        cd = base + boff
        bmagic, blen, _version, _flags, hash_off, _ident, nspecial = struct.unpack_from(">IIIIIII", mo, cd)
        hash_size, hash_type = mo[cd + 36], mo[cd + 37]
        if bmagic != CSMAGIC_CODEDIRECTORY or boff + blen > length or HASH_SIZES.get(hash_type) != hash_size:
            raise ValueError("bad CodeDirectory")
        if hash_size * nspecial > hash_off or hash_off > blen:
            raise ValueError("special slots outside the CodeDirectory")
        out.append((cd, hash_type, hash_size, nspecial, hash_off))
    return out


def special_slots(mo, slot):
    """{hashType: hash} of special slot -SLOT in each CodeDirectory; a slot
    that is absent or all zeros (not bound) is left out."""
    out = {}
    for cd, hash_type, hash_size, nspecial, hash_off in code_directories(mo):
        if slot <= nspecial:
            h = bytes(mo[cd + hash_off - slot * hash_size:cd + hash_off - (slot - 1) * hash_size])
            if any(h):
                out[hash_type] = h
    return out


def bind_special_slots(mo, data_by_slot):
    """MO (a thin Mach-O signed ad hoc) with special slot -N of every
    CodeDirectory set to the hash of data_by_slot[N], or to zeros (not
    bound) where that is None. Only the slot hashes
    change: an ad hoc signature has no CMS signature over the CodeDirectory,
    so the result is self-consistent. ValueError if a CodeDirectory has no
    room for the slot or the slice is not signed."""
    out = bytearray(mo)
    cds = code_directories(mo)
    if not cds:
        raise ValueError("not signed")
    for cd, hash_type, hash_size, nspecial, hash_off in cds:
        for slot, data in data_by_slot.items():
            if slot > nspecial:
                raise ValueError("CodeDirectory has %d special slots, slot -%d needed" % (nspecial, slot))
            at = cd + hash_off - slot * hash_size
            out[at:at + hash_size] = bytes(hash_size) if data is None else digest(hash_type, data)
    return bytes(out)
