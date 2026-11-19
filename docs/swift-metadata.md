# Swift metadata in a translated process

How Swift type metadata, witness tables and conformances work when an ARM64 program runs under hao against the
native x86_64 Swift runtime, what a metadata request costs, and how prespecialized metadata in the shared cache
applies. Code paths are in this repository; the boundary itself is described in [architecture](architecture.md).

## One Swift runtime

A translated process has one Swift runtime, the native x86_64 `libswiftCore` from the shared cache. hao does not
translate it and keeps no ARM copy of it.

- **Guest records are registered with it.** dyld never loads a guest image, so the native runtime would not see
  its Swift sections. `src/swift.c` hands each guest image's `__swift5_protos`, `__swift5_types` and
  `__swift5_proto` records to the runtime's registration entry points, the same ones static linking uses. The
  records are relative pointers and stay valid wherever the image is mapped.
- **Where metadata lives.**
  - The native runtime builds the metadata it instantiates: generic and resilient types, a guest's included, from
    the guest's patterns, plus singleton and foreign initialization.
  - Metadata the compiler emitted statically (a non-generic guest type's metadata, its value witness table, its
    witness table patterns) stays in the guest image's data, and the native runtime reads it there.
  - Both use the 64-bit Swift metadata layout, which is the same on ARM64 and x86_64. What differs is the layout
    of some values (extra inhabitants, multi-payload enum tags, Optional over a guest enum), which
    `src/swiftvalue.c` and `src/swiftvalue/` convert at the boundary.
- **Lookups are native.** Type lookup by mangled name, conformance lookup and generic instantiation all run in
  the native runtime, for requests from both sides.

## Guest functions that metadata names

Swift metadata names functions through 32-bit relative pointers inside the guest image:
- type metadata accessors;
- the instantiation and completion functions of generic metadata patterns, and singleton/foreign initialization;
- generic witness-table instantiators and resilient witnesses;
- associated type and conformance accessors, and symbolic references to accessor functions in mangled names;
- layout-string functions;
- class vtable and override entries;
- the `@main` entry point.
The native runtime calls these directly. A relative pointer cannot reach hao's code cache, which can lie anywhere
in the address space.

Before an image's records are registered, `src/swiftmeta.c` walks the metadata ABI from each descriptor. It
rewrites every such field whose target is a function start in the same image (`LC_FUNCTION_STARTS`) to point at
a thunk in an island mapped within ±2 GB of the image. Absolute slots (value witness tables, witness table
patterns) get a plain thunk address instead. A call through either enters the guest through the callback bridge
like any other native-to-guest call.

Kinds whose signature the metadata ABI fixes get exact entries (`swiftmeta.c`, `kinds`):
- metadata accessors and prespecialized accessors;
- instantiation and completion functions, and witness-table instantiators;
- destroyers and resilient-class relocation;
- symbolic-reference functions, enum-tag and layout-string accessors;
- value witnesses.
Witnesses and vtable entries get the signature of their protocol requirement or method where one is known.

Two kinds of field keep the fault path, and both are counted: a field the walk cannot resolve is left alone, and
so is a field whose island cannot be placed within 2 GB (logged). A call through such a field reaches the
non-executable guest page and takes the fault path. That path converts the arguments the same way, at the cost of
a kernel round trip.

## Witness tables and conformances

A guest conformance is registered like any other. When the native runtime instantiates a witness table for it,
the table's slots are entry points of the guest's witnesses (island entries for relative witnesses, thunks for
the pattern's absolute slots), so native protocol dispatch (SwiftUI calling a
guest view's `body`, a value witness copying a guest value) enters translated code through the bridge.
Conformances of native types to native protocols never leave native code.

## What a metadata request costs

A generic metadata request (`swift_getGenericMetadata` and the other metadata entry points) is a native call.
Its cost depends on who makes it, and on whether the type's closure (the generic type and every type argument,
recursively) lies entirely in the native shared cache or includes a guest-defined type:

| Requester | Closure | Already instantiated (hit) | First request (miss) |
|---|---|---|---|
| native framework | shared cache only | native lookup | native instantiation |
| native framework | includes a guest type | native lookup | native instantiation + callbacks into guest accessors and instantiation functions |
| guest code | shared cache only | one guest-to-native call + native lookup | one guest-to-native call + native instantiation |
| guest code | includes a guest type | one guest-to-native call + native lookup | one guest-to-native call + native instantiation + callbacks |

- A hit re-requests a type that already exists. UI code makes many more hits than misses, so the per-call
  crossing for guest requesters matters on warm paths.
- A miss on a guest closure is where hao costs more than a native process. Each guest accessor, instantiation
  function or witness-table instantiator the runtime calls is a native-to-guest callback that then runs
  translated code.
- The split between the four rows depends on the program. It has not been measured on Intel hardware; this
  page gives the structure, not numbers.

## Prespecialized metadata

The Swift runtime can be given metadata computed ahead of time. The shared cache carries a prespecialization
library (`libswiftPrespecialized`) whose map holds complete metadata for chosen generic instantiations, keyed by
the generic descriptor and its arguments (pointer keys) or by mangled name. `swift_getGenericMetadata` consults
that map before it instantiates. On a match it returns the stored object and skips instantiation.

Under hao:
- **The map is consulted natively for every requester.** A guest request reaches the same native
  `swift_getGenericMetadata`, with the same native argument metadata pointers, and so hits the same entries. No
  hao code is involved in the lookup.
- **Only closures entirely in the shared cache can be prespecialized.** A guest-defined type's descriptor and
  metadata live in the guest image: an ARM64 file outside the x86_64 shared cache, mapped at a different address
  in each process. A cache entry cannot point at it. A native app's own types are in the same position, so
  prespecialization covers framework types (SwiftUI, Combine, Foundation and standard library instantiations over
  them), not the program's own.
- **Requirements a cache with prespecialized metadata must meet for hao:**
  - Optional values cross the boundary through `src/swiftvalue.c`, which recognizes libswiftCore's own Optional
    value witnesses to repair an outer nil. A prespecialized Optional must use those witnesses (or swiftvalue
    must know its own). Otherwise its nils cross unrepaired.
  - The swiftvalue fast paths compare metadata and witness pointers with native libswiftCore symbols. Entries with
    other witnesses fail those checks closed: correct, but slower.
  - Interface units and value contracts are keyed by a library's LC_UUID. A cache that changes libswiftCore's code
    without changing its UUID is a different input. Generated tables and contracts record the UUID of the shared
    cache they were generated for (`generated.lock`), and they have to be regenerated for a changed cache.

## Checking a cache with prespecialized metadata

`make catalog-gate CACHE=DIR [BASE_CACHE=DIR [ACCEPT_EDITS=RECIPE.json]] [UNITS=DIR]` (`tools/hao_catalog_gate.py`) checks the three
requirements above on an x86_64 cache before hao runs on it. It reads the catalog of `libswiftPrespecialized`
(`_swift_prespecializationsData`, versions 1.x): its string-keyed map, the pointer-keyed hash table libswiftCore
searches (+24, in dyld's Swift table format, offsets from the cache base) and the flat pointer-keyed list whose key
symbols give the names. A rebuilt cache may add entries to the table only, so every entry of either is checked.
1. **Optional witnesses (fails closed).** Each prespecialized Optional's value witnesses (slots 0-7 and 11-13)
   must be the generic ones in the instantiation pattern of `$sSqMn` in the same cache, which every run-time
   Optional shares. Any other witness is listed with the image that holds it.
2. **Fast paths (a count).** Value types whose witness table is not one of libswiftCore's exported tables and
   whose witnesses are not all in libswiftCore's `__TEXT` cannot match the pointer checks; they take the general
   path.
3. **Bound inputs (fails closed).** Every cache-derived table in `generated.lock` must name the cache's UUID.
   Every value contract whose x86_64 image is in the cache must name its UUID and its anchors must still hash to
   the pinned values. With `UNITS`, every interface unit must match an image. With `BASE_CACHE` (the cache the
   inputs were made for), an image whose UUID is unchanged but whose `__TEXT` differs fails every unit and
   contract bound to it. With `ACCEPT_EDITS`, a reviewed list of cache edits (address, bytes before and after),
   an image changed only inside those edits passes, and the gate names the edits. When `CACHE` is a rebuilt
   family (its cache UUID differs from `BASE_CACHE`'s), the builder places and links every image again, so bytes
   are not compared: an image must keep its LC_UUID, its `__TEXT` size and every export's offset from its base.
   The cache-derived tables and value contracts still name the old cache and fail until they are regenerated
   and re-proved for the new one.

On the 26B131 cache (262FA161), which already carries Apple's catalog: 2762 entries (1574 structs, 1188 classes,
no Optionals or enums); gate 1 passes trivially; 505 structs miss the fast paths (their witnesses are in SwiftUICore,
the concurrency library and others); gate 3 passes (9 cache tables, the Optional<Data> contract's 4 anchors,
1896 units). `host/catalog-gate` tests each failure on a synthetic cache.

With the two libswiftCore edits that keep prespecialized metadata on (the calls to
`_dyld_shared_cache_some_image_overridden()` in its prespecialization setup replaced by a zero) and the 113
name-to-descriptor slots that point into libraries loaded from disk nulled, on the same cache: all gates pass with
`ACCEPT_EDITS` set to that edit list; without it, gate 3 fails libswiftCore's interface unit, as it should for
changed code under an unchanged UUID.

## Recording the Swift keys a program meets

The value walk looks each Swift type it meets up in its facts table (`src/swiftwalk_gen.inc`). hao counts every
metadata it looks up once per process, by the table's answer and by where the metadata lies, and `--stats` prints
the counts (`hao-swift-keys: met=N same=N ... none=N ...; cache=N guest=N heap=N`). With `HAO_SWIFT_KEYS=1`
(a printing switch, honoured in guarded processes too) or the boot argument `hao_swift_keys=1` (every process),
each one is also written once where hao's diagnostics go:

    hao-swift-key {"v":1,"process":P,"key":K,"row":R,"where":W,"kind":N,"size":N,"stride":N,"flags":N,"xi":N,"x86_cache":U}

- `key`: the table key, the mangled name without `$s` and with private contexts' addresses removed (the key
  `hao-swift-audit` records use);
- `row`: `same`, `dynamic`, `convert` or `refuse` (a native row), `guest`, `guest-incomplete` or `guest-optional`
  (a guest-defined type's row, `src/swiftguest_gen.inc`), `none` (no row), `layout` (a row whose size, stride,
  flags or XI count is not the live type's), `identity` (a SAME row the identity check declined), `other-cache`
  (the table is for another x86_64 cache);
- `where`: `cache` (inside a section of a shared cache image that is not zero-fill: static or prespecialized
  metadata), `guest` (a guest image's type) or `heap` (made at run time; libswiftCore's first allocations, in
  its zero-fill initial pool inside the cache range, count here);
- no address and no path.

`tools/hao_swift_keys.py LOG...` reads these records and `hao-swift-audit` records from logs and reports
coverage: keys with a row against keys looked up, the uncertified ones, keys under another cache that
`--walk-table src/swiftwalk_gen.inc` lists, and keys by where their metadata lay; with `--cache` (read as
`hao_catalog_gate.py` does) or `--catalog-names`, which heap keys a catalog names. `--tsv` writes one row per key,
`--specs` the keys without a row and those of uncertified values as walk-facts generator input. Limits: only
types the walk looks up are recorded (values crossing a native boundary that need certifying), not every
metadata request; conformances are seen only as the existential types that carry them.

## Where prespecialized metadata comes from

Prespecialized metadata is part of the x86_64 shared cache the image installs. hao-runtime neither builds nor
loads a catalog. A cache with prespecialized metadata is an input like any other cache: it is identified by its
UUID, its contents are generated for one macOS build, and its artifact is pinned by hash in the image's inputs.
Apple-derived bytes are not stored in the repositories: they are reproduced from a recipe and checked against
the pinned hashes.
