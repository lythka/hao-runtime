"""hao_codesig: what a Mach-O slice's embedded code signature says about loading libraries.

Pure Python, like hao_fat.py (the image builder has no developer tools). It
reads the slice's LC_CODE_SIGNATURE SuperBlob: the CodeDirectory's flags,
platform identifier and Team ID, and the entitlements. src/libval.c reads
the same signature at run time, and the two must agree: parse() here is
hao_codesig_parse there, rule for rule (the one LC_CODE_SIGNATURE, the
CodeDirectory of highest hash rank, the sealed blobs), check() is
hao_slice_verify (page hashes, nothing outside the hashed range), and
tests/tools/test_codesig.py runs both over the fuzz corpus and requires the
same verdict, cdhash and policy from each.

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
CSMAGIC_REQUIREMENTS = 0xFADE0C01
CSMAGIC_EMBEDDED_DER_ENTITLEMENTS = 0xFADE7172
CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT = 0xFADE8181
CSSLOT_CODEDIRECTORY, CSSLOT_REQUIREMENTS, CSSLOT_ENTITLEMENTS, CSSLOT_DER_ENTITLEMENTS = 0, 2, 5, 7
CSSLOT_ALTERNATE_CODEDIRECTORIES = range(0x1000, 0x1005)
# The blobs a CodeDirectory seals in its special slots, with their magic: requirements, entitlements (XML and
# DER) and the four launch constraints (self, parent, responsible, library).
SEALED = ((CSSLOT_REQUIREMENTS, CSMAGIC_REQUIREMENTS), (CSSLOT_ENTITLEMENTS, CSMAGIC_EMBEDDED_ENTITLEMENTS),
          (CSSLOT_DER_ENTITLEMENTS, CSMAGIC_EMBEDDED_DER_ENTITLEMENTS), (8, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT),
          (9, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT), (10, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT),
          (11, CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT))
HASH_RANK = {1: 1, 3: 2, 2: 3, 4: 4}        # xnu hashPriorities: SHA-1 < SHA-256/20 < SHA-256 < SHA-384
CS_MAX_BLOB = 40 << 20                      # xnu's CS_MAX_BLOB_SIZE
# Special slots (hashes of data outside the code pages, at -N before the code
# hashes): -1 the Info.plist (a bundle's Contents/Info.plist, or a tool's
# __TEXT,__info_plist), -3 the resource seal (a bundle's
# _CodeSignature/CodeResources).
CSSLOT_INFOSLOT, CSSLOT_RESOURCEDIR = 1, 3
CS_REQUIRE_LV, CS_RUNTIME = 0x2000, 0x10000
DLV = "com.apple.security.cs.disable-library-validation"
DYLD_ENV = "com.apple.security.cs.allow-dyld-environment-variables"


def signature_range(mo):
    """(offset, size) from the LC_CODE_SIGNATURE of a thin 64-bit Mach-O,
    None without one; ValueError if the load commands are malformed
    (hao_fat.load_commands), the command is too short to hold the range, or
    there is more than one (src/macho.c hao_macho_check_cmds refuses the
    same: every reader must mean the same signature)."""
    sig = None
    for cmd, off, size in hao_fat.load_commands(mo, "Mach-O"):
        if cmd == LC_CODE_SIGNATURE:
            if size < 16:
                raise ValueError("Mach-O: short LC_CODE_SIGNATURE")
            if sig is not None:
                raise ValueError("Mach-O: more than one LC_CODE_SIGNATURE")
            sig = struct.unpack_from("<II", mo, off + 8)
    return sig


def _code_directory(b):
    """The fields of the CodeDirectory blob B, checked as src/libval.c cd_valid
    checks them (xnu cs_validate_codedirectory, plus no scatter vector and a
    4 KiB or 16 KiB page size); ValueError otherwise."""
    if len(b) < 44:
        raise ValueError("malformed CodeDirectory")
    magic, length, version, flags, hash_off, ident_off, nspecial, ncode, limit = struct.unpack_from(">9I", b)
    hash_size, hash_type, platform, page_shift = b[36], b[37], b[38], b[39]
    scatter = struct.unpack_from(">I", b, 44)[0] if version >= 0x20100 and len(b) >= 48 else 0
    team_off = struct.unpack_from(">I", b, 48)[0] if version >= 0x20200 and len(b) >= 52 else 0
    limit64 = struct.unpack_from(">Q", b, 56)[0] if version >= 0x20300 and len(b) >= 64 else 0
    if magic != CSMAGIC_CODEDIRECTORY:
        raise ValueError("malformed CodeDirectory")
    hs = HASH_SIZES.get(hash_type)
    if not hs or hash_size != hs:
        raise ValueError("CodeDirectory hash type %d, size %d" % (hash_type, hash_size))
    if page_shift not in (12, 14):
        raise ValueError("CodeDirectory page size 2^%d" % page_shift)
    if len(b) < hash_off or hash_off // hs < nspecial or (len(b) - hash_off) // hs < ncode:
        raise ValueError("CodeDirectory hash slots outside it")
    if scatter:
        raise ValueError("scattered CodeDirectory")
    if not ident_off or ident_off >= len(b) or b.find(b"\0", ident_off) < 0:
        raise ValueError("CodeDirectory identifier malformed")
    if team_off and (team_off >= len(b) or b.find(b"\0", team_off) < 0):
        raise ValueError("CodeDirectory team identifier malformed")
    team = b[team_off:b.index(b"\0", team_off)] if team_off else b""
    return {"length": length, "flags": flags, "platform": platform, "hash_type": hash_type, "hash_size": hash_size,
            "page_shift": page_shift, "hash_off": hash_off, "n_special": nspecial, "n_code_slots": ncode,
            "code_limit": limit64 or limit, "ident": b[ident_off:b.index(b"\0", ident_off)], "team": team or None}


def _xml_true(p, key):
    """<key>KEY</key> followed by <true in the XML plist P (src/libval.c xml_true)."""
    pat = b"<key>" + key.encode() + b"</key>"
    i = p.find(pat)
    if i < 0:
        return False
    j = i + len(pat)
    while j < len(p) and p[j] in b" \t\n\r":
        j += 1
    return p[j:j + 5] == b"<true"


def _der_true(p, key):
    """UTF8String KEY then BOOLEAN TRUE in the DER entitlements P (src/libval.c der_true)."""
    k = key.encode()
    if len(k) > 127:
        return False
    pat = bytes([0x0C, len(k)]) + k + b"\x01\x01"
    i = p.find(pat)
    while i >= 0:
        if i + len(pat) < len(p) and p[i + len(pat)] != 0:
            return True
        i = p.find(pat, i + 1)
    return False


def parse(blob):
    """The embedded signature BLOB (a SuperBlob) as src/libval.c
    hao_codesig_parse reads it, which is how xnu validates one: every index
    entry in range, CodeDirectories valid and of distinct hash types, the
    one of highest hash rank used, at most one blob per sealed slot with the
    right magic, and each sealed blob matching its special slot (an absent
    one: no slot or an all-zero one). Returns the CodeDirectory's fields
    (_code_directory) with "cd" (its bytes), "cdhash", "ents" and "der_ents"
    (the sealed entitlement blobs, headers included, or None) and "dlv".
    ValueError if the signature is malformed."""
    blob = bytes(blob)
    if len(blob) < 12:
        raise ValueError("not an embedded signature")
    magic, total, count = struct.unpack_from(">III", blob)
    if magic != CSMAGIC_EMBEDDED_SIGNATURE or total > len(blob) or total < 12:
        raise ValueError("not an embedded signature")
    if (total - 12) // 8 < count:
        raise ValueError("signature index past its end")
    index, best, best_rank, ranks, primary, seen = [], None, 0, set(), False, set()
    for k in range(count):
        typ, off = struct.unpack_from(">II", blob, 12 + 8 * k)
        if off > total or total - off < 8:
            raise ValueError("signature blob %d outside the signature" % k)
        bmagic, blen = struct.unpack_from(">II", blob, off)
        if blen > total - off or blen < 8:
            raise ValueError("signature blob %d outside the signature" % k)
        index.append((typ, off, bmagic, blen))
        if typ == CSSLOT_CODEDIRECTORY or typ in CSSLOT_ALTERNATE_CODEDIRECTORIES:
            cd = _code_directory(blob[off:off + blen])
            primary |= typ == CSSLOT_CODEDIRECTORY
            rank = HASH_RANK[cd["hash_type"]]
            if rank in ranks:
                raise ValueError("two CodeDirectories with hash type %d" % cd["hash_type"])
            ranks.add(rank)
            if rank > best_rank:
                best_rank, best = rank, (off, cd)
        elif typ in dict(SEALED):
            if bmagic != dict(SEALED)[typ]:
                raise ValueError("signature blob for slot %d has magic %#x" % (typ, bmagic))
            if typ in seen:
                raise ValueError("two signature blobs for slot %d" % typ)
            seen.add(typ)
    if not primary or best is None:
        raise ValueError("no CodeDirectory")
    at, out = best
    cd = blob[at:at + out["length"]]
    hs, sealed = out["hash_size"], {}
    for slot, want in SEALED:
        b = next((blob[off:off + blen] for typ, off, bmagic, blen in index if typ == slot and bmagic == want), None)
        if slot > out["n_special"]:
            if b is not None:
                raise ValueError("signature blob for slot %d is not sealed by the CodeDirectory" % slot)
            continue
        have = cd[out["hash_off"] - slot * hs:out["hash_off"] - (slot - 1) * hs]
        if b is None:
            if any(have):
                raise ValueError("signature blob for slot %d is missing" % slot)
            continue
        if digest(out["hash_type"], b) != have:
            raise ValueError("signature blob for slot %d does not match its hash" % slot)
        sealed[slot] = b
    out["cd"] = cd
    out["cdhash"] = digest(out["hash_type"], cd)[:20]
    out["ents"], out["der_ents"] = sealed.get(CSSLOT_ENTITLEMENTS), sealed.get(CSSLOT_DER_ENTITLEMENTS)
    out["dlv"] = (_xml_true(out["ents"][8:], DLV) if out["ents"] is not None else
                  out["der_ents"] is not None and _der_true(out["der_ents"][8:], DLV))
    return out


def check(mo):
    """What hao decides about the signature of the thin 64-bit Mach-O MO
    before it runs or maps it (src/libval.c hao_slice_verify): parse()'s
    result with "verdict" "valid" when the structure holds, every page hash
    matches at the CodeDirectory's own page size, and nothing in the slice
    but the signature lies outside the hashed range; {"verdict": "invalid",
    "why": ...} otherwise; {"verdict": "unsigned"} without a signature.
    ValueError if the load commands are malformed or name more than one
    signature (hao refuses to load such a slice at all)."""
    sig = signature_range(mo)
    if not sig or not sig[1]:
        return {"verdict": "unsigned"}
    off, size = sig
    try:
        if off + size > len(mo) or size > CS_MAX_BLOB:
            raise ValueError("code signature [%#x, +%#x) outside the slice" % (off, size))
        out = parse(mo[off:off + size])
        limit, page = out["code_limit"], 1 << out["page_shift"]
        if limit < off or limit > len(mo) or len(mo) > max(limit, off + size):
            raise ValueError("it covers [0, %#x) of a %#x-byte slice whose signature is at %#x" % (limit, len(mo), off))
        if (limit + page - 1) // page != out["n_code_slots"]:
            raise ValueError("the CodeDirectory hashes %d pages of a %d-byte code range" % (out["n_code_slots"], limit))
        hs, slots = out["hash_size"], out["hash_off"]
        for i in range(out["n_code_slots"]):
            if digest(out["hash_type"], mo[i * page:min((i + 1) * page, limit)]) != out["cd"][slots + i * hs:slots + (i + 1) * hs]:
                raise ValueError("page %d (offset %#x) does not match its hash" % (i, i * page))
    except ValueError as e:
        return {"verdict": "invalid", "why": str(e)}
    out["verdict"] = "valid"
    return out


def info(mo):
    """dict(signed, flags, platform, team, entitlements, dlv) of a thin
    64-bit little-endian Mach-O, or None if it is not one: what its
    signature says, from the CodeDirectory hao itself uses (parse). A
    signature hao would call malformed reads as unsigned; the page hashes
    are not checked here (check does that)."""
    if len(mo) < 32 or struct.unpack_from("<I", mo)[0] != 0xFEEDFACF:
        return None
    out = {"signed": False, "flags": 0, "platform": 0, "team": None, "entitlements": {}, "dlv": False}
    try:
        sig = signature_range(mo)
        if not sig or not sig[1] or sig[0] + sig[1] > len(mo) or sig[1] > CS_MAX_BLOB:
            return out
        cs = parse(mo[sig[0]:sig[0] + sig[1]])
    except ValueError:
        return out
    out.update(signed=True, flags=cs["flags"], platform=cs["platform"], dlv=cs["dlv"],
               team=cs["team"].decode(errors="replace") if cs["team"] else None)
    if cs["ents"] is not None:
        try:
            ents = plistlib.loads(cs["ents"][8:])
            if isinstance(ents, dict):
                out["entitlements"] = ents
        except Exception:
            pass
    return out


def library_validated(i):
    """True if the program, run natively, loads only platform or same-team libraries
    (src/libval.c hao_codesig_library_validated)."""
    if not i or not i["signed"] or i["dlv"]:
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
