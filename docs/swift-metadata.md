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
- **Metadata objects are native.** The native runtime builds and owns the metadata of every type, a guest-defined
  type included, using the native x86_64 layout. Guest code reads size, stride, flags and value witnesses from
  these objects. Where an ARM64 value and an x86_64 value of the same type differ (extra inhabitants, multi-payload
  enum tags, Optional over a guest enum), `src/swiftvalue.c` and `src/swiftvalue/` convert at the boundary.
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
a thunk in an island mapped within ±2 GB of the image. A call through an island enters the guest through the
callback bridge like any other native-to-guest call. Kinds whose signature the metadata ABI fixes (accessors,
instantiation, completion, instantiators, destroyers, layout-string functions) get exact entries. Witnesses and
vtable entries get the signature of their protocol requirement or method where one is known. A field the walk
cannot resolve is left alone and counted. A call through it reaches the non-executable guest page and takes the
fault path, which converts the arguments the same way at the cost of a kernel round trip.

## Witness tables and conformances

A guest conformance is registered like any other. When the native runtime instantiates a witness table for it,
the table's slots are the island entries of the guest's witnesses, so native protocol dispatch (SwiftUI calling a
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

## Where prespecialized metadata comes from

Prespecialized metadata is part of the x86_64 shared cache the image installs. hao-runtime neither builds nor
loads a catalog. A cache with prespecialized metadata is an input like any other cache: it is identified by its
UUID, its contents are generated for one macOS build, and its artifact is pinned by hash in the image's inputs.
Apple-derived bytes are not stored in the repositories: they are reproduced from a recipe and checked against
the pinned hashes.
