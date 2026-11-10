# hao-contract-26A428 0.1.0-analytical.5

Interface contracts for macOS 27.0 (26A428): 490 records in 110 interface units,
one unit per native library UUID, plus the paired Swift thunk library description
(`swift-thunks.json`).

This release is a delta. It adds 364 scalar argument and result contracts and
repeats 126 records from its predecessor unchanged. It is a candidate, not the
contract set selected for an image.

## Using it

Merge the delta into the exact 26A428 base named in `release.json`:

- Merge by library UUID and symbol. Identical records are no-ops; differing
  records are conflicts and must be rejected.
- Never replace a base interface unit with a delta file.
- Keep the existing thunk libraries.
- Rebuild AOT caches for the runtime you package with the merged result.

`make` does not read this directory. See [Releases](../../../../docs/releases.md)
for packaging.

## Scope

Each record is bound to the library UUID, CPU subtype and exact code bytes it was
derived from (arm64e guest, x86_64 host). The release contains only generated
contract data: no Apple binaries, extracted instructions or compiler output.
These contracts describe calling conventions only; they do not by themselves make
an application compatible.
