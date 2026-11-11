# Releases

`tools/hao_release.py` validates and materializes a versioned hao release
(schema 1). A release is an assembly of components, each with its own revision
and a SHA-256-pinned manifest; the assembly pins the complete selection.

| Component | Contents | Depends on |
|---|---|---|
| runtime | `/usr/libexec/hao`, `libhao.dylib`, `libhao_identity.dylib` | |
| compatibility | interface units, trusted cdhash list, optional async provider | runtime |
| image plan | HaoExec, install helpers, embedding stubs and manifest, exact replacements | runtime, compatibility |
| AOT recipe | runtime-bound AOT caches and how to generate them | runtime, compatibility |
| build record | the original build record and logs | |

The runtime consumes interface v2 and AOT v6. An AOT v6 cache names the
runtime that generated it and the cdhash of the guest it was translated from
(`include/hao/aot.h`); the AOT recipe's `format_version` is 6 and each guest
binding records the header's `id_kind`, `id_hash` and `code_id`. Caches install
into a system cache directory, which serves signed code only, so `check`
rejects a binding whose identity is not a cdhash. AOT v5 caches and recipes are
not accepted: regenerate them with the selected runtime. Native library UUIDs,
the native-cache identity, OS build, kernel input hash, ARM64e guest and x86_64
host are explicit. The initial CPU profile requires SSE2, CMPXCHG16B and
AVX-capable hardware; the runtime and translated code use at most x86-64-v2
(SSSE3, SSE4.1, SSE4.2, POPCNT), and `hao` refuses to start on a CPU without
the profile (AVX is not required under Rosetta, where the tests run). The target JSON passed to the tool is a declaration by
the builder, not a hardware probe.

This is a development interface, not an authenticated updater: hashes are not
publisher signatures, and schema acceptance is not behavioural acceptance.

## What is distributed

A release bundle contains hao-owned code, generated contract data, metadata and
recipes. It never contains Apple binaries, modified Apple binaries or
translated Apple code. Embedded executable stubs, the dyld fix and AOT
translations are `generate-locally` entries: their expected hashes and recipes
are in the bundle, their bytes are not. The validator rejects such entries
marked for shipping, and rejects extra objects.

## Usage

```sh
python3.14 -B tools/hao_release.py check BUNDLE --target TARGET.json
python3.14 -B tools/hao_release.py generate BUNDLE --system OS_ROOT \
  --native-cache X86_CACHE --build HAO_BUILD --out LOCAL_GENERATED
python3.14 -B tools/hao_release.py preflight BUNDLE --target TARGET.json \
  --system OS_ROOT --native-cache X86_CACHE --kernel-input BOOT_KC \
  --generated LOCAL_GENERATED
python3.14 -B tools/hao_release.py materialize BUNDLE --generated LOCAL_GENERATED \
  --out LOCAL_PAYLOAD
```

- `check` validates the bundle's manifests, dependencies, interface structure
  and AOT header bindings against the target.
- `generate` builds the Apple-derived outputs from the user's own System root
  with fixed hao tools; it never runs a command from a manifest. It needs
  macOS, Python 3.14, `timeout`, Apple's compiler and signing tools and a hao
  build matching the bundle's runtime (including `hao-stub` and
  `front/stub.o`). Every output must match its recorded hash. Repeat
  `--only PAYLOAD_PATH` to reproduce part of it.
- `preflight` reads the System without writing it and checks source or
  already-applied hashes, bundle resource identities, embedding results, AOT
  source bindings, path exclusions, and the native-cache and kernel-input
  identities.
- `materialize` copies the verified inputs into a new staging directory and
  renames it into place, in the layout `tools/hao-install.py` installs.

Generated and materialized outputs carry a local-only marker: they contain
Apple-derived code and must not be redistributed. None of these commands
modifies a System volume, kernel or EFI partition; installation, sealing and
kernel stamping are steps of the image build.

## Async provider

The compatibility component may contain
`root/usr/lib/hao/libhao_async_contract.dylib` and
`root/usr/lib/hao/async-contract.json`, always together. The descriptor is
schema 1, kind `hao-async-contract`, with `abi: 1`, `identifier`, `version`,
exact `os_build`, `guest_arch`, `host_arch`, `library_sha256` and
`library_uuid`. The validator checks the target, hash, UUID and x86_64 dylib
format without executing it. Such a package requires the runtime component to
declare `async_contract_package_abis: [1]` in its build recipe.

`tools/hao-image-payload --async-provider DIR` validates a provider and
descriptor against the input System's build and stages them beside
`libhao.dylib`. See the [async contract interface](async-contract.md).

## Contract export for 26A428

[`releases/contracts/26A428/0.1.0-analytical.5`](../releases/contracts/26A428/0.1.0-analytical.5/README.md)
is a generated contract delta for macOS 27 build 26A428: 490 records in 110
UUID interface units, plus a paired Swift companion library. Its manifests
record the generator's provenance and hashes. `make` does not consume or
install it.

To use it in an image, start from the exact 26A428 base named in
`release.json`, merge by library UUID and symbol, keep identical rows as
no-ops, reject conflicting rows, and keep existing thunk libraries. Do not
replace base units with delta files. Rebuild runtime-bound AOT caches for the
selected runtime and package the complete compatibility input with it.

A unit is used only for a loaded library with its UUID. `hao-image-payload
--units` therefore fails if a unit names no image of the image's
`dyld_shared_cache_x86_64` (`tools/hao_ifunit.py match UNITDIR CACHE`, which
also checks an installed `/usr/lib/hao/if` copy against a cache). Units
generated for another build, or for `dyld_shared_cache_x86_64h`, are refused.

## Allowlist

`releases/runtime-files.txt` lists every file this repository may contain.
`python3.14 tools/check_distribution.py` rejects any other tracked path;
`--history` checks every commit reachable from the selected ref against the
allowlist that commit carried (`distribution/runtime-files.txt` before the
file moved to `releases/`).
