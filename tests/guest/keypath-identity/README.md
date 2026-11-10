# Computed key path identity across direct and indirect references

```sh
python3.14 -B tests/guest/keypath-identity/run.py
```

`HAO` selects the runtime, `OUT` a separate output folder, and the optional
`HAO_BASE` a baseline runtime expected to fail. The runner builds signed native
ARM, native Intel and guest command-line probes; no GUI is launched.

## What it tests

Swift computed key path components identify their property by an ID that can be
a direct relative pointer to the getter or an indirect pointer through a
function-pointer slot. Two key paths naming the same property must compare
equal and hash equally whichever form each uses (Swift `KeyPath.swift`:
`ComputedPropertyID` equality and hashing compare the identity value and kind).

The fixture takes two compiler-emitted computed key path patterns with
identical storage and sets their identity fields to name one property: one
direct, one indirect through a static slot authored as a linker rebase, so it
goes through hao's function-slot pass. It checks the pattern layout before
changing its own probe bytes, then signs the binaries. Both native
architectures read the value through both key paths, compare them equal and
produce equal hashes; the guest must do the same under JIT, interpreter,
verified AOT and with fixup thunks disabled.

## How hao handles it

`hao_fnslots_address_taken` treats direct computed pointer-ID references in
`__const` as address-taking uses, so the slot policy keeps the function's guest
address in its absolute slots too. Stored-property and vtable integer IDs,
indirect IDs and callable ID resolvers are excluded. The header masks are the
ones `src/swiftmeta.c` already uses. The scan is limited to compiler-emitted
constant data; key path patterns built at run time are not covered.
