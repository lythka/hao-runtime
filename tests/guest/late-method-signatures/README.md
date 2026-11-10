# Aggregate methods on late Swift classes

```sh
python3 tests/guest/late-method-signatures/run.py
```

`HAO` selects the runtime and `OUT` an isolated build directory. Every
executable is ad-hoc signed and every run has a timeout. Only Foundation is
loaded. The default run builds both relative and absolute Objective-C method
lists; `ABSOLUTE_METHODS=0` or `1` selects one format. `results.json` records
the outcomes, with each run's stdout and stderr beside it.

## What it tests

A native Objective-C helper calls `constrainFrameRect:toScreen:` on three Swift
objects: a non-generic class with generic ancestry, a generic class instance,
and a class inheriting that generic method. Each identity method receives and
returns two distinct 32-byte rectangles, and the helper checks every byte.
This is the ABI shape of AppKit window overrides, without a window server.

A late class is realized after the function-pointer slot pass, which can
replace its raw method IMP with a plain native entry. Realization must upgrade
that entry using the method's Objective-C encoding: `CGRect` is an HFA on ARM,
but a stack argument and indirect result on x86_64. Generic classes need the
same upgrade when their generic pattern contains absolute method entries
(`-no_objc_relative_method_lists`), because the slot pass can replace them
before the generic metadata allocator copies the pattern. Relative entries do
not pass through the slot pass.

The test also calls a `CGRect` getter on generic and inherited Swift objects
whose guest override calls its native superclass getter, a
native-to-guest-to-native nesting. A third getter subclasses a native Swift
library built with library evolution: Swift allocates that guest subclass's
metadata outside the guest image, so method registration identifies the class
by its descriptor, which the guest owns, rather than by its address.

All format and mode combinations must match native ARM and native Intel under
JIT, interpreter and AOT (with the cache load checked), and with fixup thunks
disabled.
