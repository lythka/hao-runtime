"""patch.py IN OUT EDIT...: copy an AOT v6 cache (include/hao/aot.h) with edits.

After the edits the payload hash is recomputed (unless an edit says
nohash), so each loader check can be tested on its own. Edits:
  hdr.FIELD=V        version, hdr_size, nblocks, nlinks, code_size, file_size,
                     vmsize, guest_id, payload_hash, id_kind, id_hash
  ent.FIELD=K:V      guest_off or code_off of entry K (V may be +N or -N)
  link.FIELD=K:V     site or target of link K
  flip.uuid  flip.gen  flip.code    flip one byte of that field / the first block
  flip.code_id       flip one bit of the guest code identity (a cache translated
                     from other code under the same UUID and load commands)
  plant              fill the code area (not the base slot, not link rel32s)
                     with int3, as an attacker's cache would carry its own code
  truncate=N         keep only the first N bytes (applied last)
  nohash             leave payload_hash as it is
"""
import struct
import sys

HDR = struct.Struct("<8sII16s16sQQIIQQQII32s")
FIELDS = ["magic", "version", "hdr_size", "uuid", "gen", "vmsize", "guest_id", "nblocks", "nlinks",
          "code_size", "file_size", "payload_hash", "id_kind", "id_hash", "code_id"]
M = (1 << 64) - 1


def hash64(b, seed=0):
    n = len(b)
    h = (seed ^ (0x9E3779B97F4A7C15 * (n + 1))) & M
    i = 0
    while n - i >= 8:
        w = int.from_bytes(b[i:i + 8], "little")
        h = ((h ^ w) * 0xff51afd7ed558ccd) & M
        h ^= h >> 29
        i += 8
    r = n - i
    w = int.from_bytes(bytes(b[i:]) + b"\0" * (8 - r), "little")
    h = ((h ^ w ^ (r << 56)) * 0xc4ceb9fe1a85ec53) & M
    return h ^ (h >> 32)


def value(old, v):
    if v.startswith("+"):
        return old + int(v[1:], 0)
    if v.startswith("-"):
        return old - int(v[1:], 0)
    return int(v, 0)


def main():
    src, dst, edits = sys.argv[1], sys.argv[2], sys.argv[3:]
    data = bytearray(open(src, "rb").read())
    h = dict(zip(FIELDS, HDR.unpack_from(data, 0)))
    # layout comes from the original header, so edits to it do not move anything
    ents_at = HDR.size
    links_at = ents_at + 8 * h["nblocks"]
    code_at = links_at + 8 * h["nlinks"]
    rehash, truncate = True, None
    for e in edits:
        if e == "nohash":
            rehash = False
        elif e.startswith("truncate="):
            truncate = int(e.split("=", 1)[1], 0)
        elif e.startswith("hdr."):
            k, v = e[4:].split("=", 1)
            h[k] = value(h[k], v) & ((1 << 32) - 1 if k in ("version", "hdr_size", "nblocks", "nlinks", "id_kind", "id_hash") else M)
        elif e.startswith("ent.") or e.startswith("link."):
            kind, rest = e.split(".", 1)
            f, kv = rest.split("=", 1)
            k, v = kv.split(":", 1)
            base = (ents_at if kind == "ent" else links_at) + 8 * int(k)
            off = base + {"guest_off": 0, "code_off": 4, "site": 0, "target": 4}[f]
            old = struct.unpack_from("<I", data, off)[0]
            struct.pack_into("<I", data, off, value(old, v) & 0xffffffff)
        elif e == "flip.uuid":
            h["uuid"] = bytes([h["uuid"][0] ^ 1]) + h["uuid"][1:]
        elif e == "flip.gen":
            h["gen"] = bytes([h["gen"][0] ^ 1]) + h["gen"][1:]
        elif e == "flip.code_id":
            h["code_id"] = bytes([h["code_id"][0] ^ 1]) + h["code_id"][1:]
        elif e == "flip.code":
            first = struct.unpack_from("<I", data, ents_at + 4)[0]
            data[code_at + first] ^= 0xff
        elif e == "plant":
            keep = set()
            for k in range(h["nlinks"]):
                site = struct.unpack_from("<I", data, links_at + 8 * k)[0]
                keep.update(range(site, site + 4))
            for i in range(16, h["code_size"]):
                if i not in keep:
                    data[code_at + i] = 0xCC
        else:
            sys.exit("patch.py: unknown edit " + e)
    if rehash:
        h["payload_hash"] = hash64(data[HDR.size:])
    HDR.pack_into(data, 0, *[h[f] for f in FIELDS])
    if truncate is not None:
        data = data[:truncate]
    open(dst, "wb").write(data)


if __name__ == "__main__":
    main()
