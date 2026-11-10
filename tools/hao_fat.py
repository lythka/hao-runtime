"""hao_fat: add an x86_64 slice to an arm64/arm64e Mach-O without lipo.

Pure Python (runs in an image builder that has no developer tools). Every
existing slice is copied byte for byte, so each keeps its own code
signature; the result is a big-endian fat32 file with 16 KiB alignment.
combine(..., drop_x86_64=True) first drops a universal file's own x86_64
slices (hao-embed --translate), so the stub becomes its only x86_64 slice.
"""
import struct

CPU_X86_64, CPU_ARM64 = 0x01000007, 0x0100000C
ALIGN_LOG2 = 14
ALIGN = 1 << ALIGN_LOG2
MH_MAGIC_64 = 0xFEEDFACF


def slices(data):
    """[(cputype, cpusubtype, bytes)] for a thin 64-bit or fat Mach-O; None otherwise."""
    if len(data) < 32:
        return None
    magic = struct.unpack_from("<I", data)[0]
    if magic == MH_MAGIC_64:
        cpu, sub = struct.unpack_from("<II", data, 4)
        return [(cpu, sub, data)]
    if magic not in (0xBEBAFECA, 0xBFBAFECA):
        return None
    wide = magic == 0xBFBAFECA
    n = struct.unpack_from(">I", data, 4)[0]
    size = 32 if wide else 20
    if 8 + n * size > len(data):
        return None
    out = []
    for k in range(n):
        base = 8 + k * size
        cpu, sub = struct.unpack_from(">II", data, base)
        if wide:
            off, ln = struct.unpack_from(">QQ", data, base + 8)
        else:
            off, ln = struct.unpack_from(">II", data, base + 8)
        if off + ln > len(data):
            return None
        out.append((cpu, sub, data[off:off + ln]))
    return out


def has_cpu(data, cpu):
    sl = slices(data)
    return bool(sl) and any(c == cpu for c, _, _ in sl)


def combine(base, extra, drop_x86_64=False):
    """base (arm64 thin or fat) + extra (thin x86_64) -> fat bytes.

    drop_x86_64: base is universal; its x86_64 slices (x86_64, x86_64h) are
    left out and the other slices keep their order and bytes."""
    a, b = slices(base), slices(extra)
    if not a or not b or len(b) != 1 or b[0][0] != CPU_X86_64:
        raise ValueError("expected an arm64 Mach-O and a thin x86_64 Mach-O")
    if drop_x86_64:
        if not any(c == CPU_X86_64 for c, _, _ in a):
            raise ValueError("base has no x86_64 slice to replace")
        a = [s for s in a if s[0] != CPU_X86_64]
    if not any(c == CPU_ARM64 for c, _, _ in a):
        raise ValueError("base has no arm64 slice")
    if any(c == CPU_X86_64 for c, _, _ in a):
        raise ValueError("base already has an x86_64 slice")
    parts = a + b
    if sum(len(p) for _, _, p in parts) + ALIGN * (len(parts) + 1) >= 1 << 32:
        raise ValueError("too large for fat32")
    header = struct.pack(">II", 0xCAFEBABE, len(parts))
    offset = ALIGN
    table, body = b"", b""
    for k, (cpu, sub, payload) in enumerate(parts):
        table += struct.pack(">IIIII", cpu, sub, offset, len(payload), ALIGN_LOG2)
        pad = -len(payload) % ALIGN if k + 1 < len(parts) else 0
        body += payload + b"\0" * pad
        offset += len(payload) + pad
    head = header + table
    return head + b"\0" * (ALIGN - len(head)) + body


def build(parts):
    """[(cputype, cpusubtype, bytes)] -> fat32 bytes, 16 KiB aligned, original order."""
    header = struct.pack(">II", 0xCAFEBABE, len(parts))
    offset = ALIGN
    table, body = b"", b""
    for k, (cpu, sub, payload) in enumerate(parts):
        table += struct.pack(">IIIII", cpu, sub, offset, len(payload), ALIGN_LOG2)
        pad = -len(payload) % ALIGN if k + 1 < len(parts) else 0
        body += payload + b"\0" * pad
        offset += len(payload) + pad
    head = header + table
    return head + b"\0" * (ALIGN - len(head)) + body


def replace_slice(data, cpu, payload):
    """Replace the slice of CPU in a fat Mach-O; other slices are copied unchanged."""
    parts = slices(data)
    if not parts or sum(c == cpu for c, _, _ in parts) != 1:
        raise ValueError("expected exactly one slice of that CPU")
    return build([(c, s, payload if c == cpu else p) for c, s, p in parts])
