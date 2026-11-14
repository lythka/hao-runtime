# Native callers of guest bundles

```sh
python3 tests/guest/native-bundle/run.py
```

Builds Foundation-only command-line fixtures. No system application, GUI
session or XPC service is launched.

## What it tests

Native `NSBundle` and `CFBundle` load code with dyld's `dlopen`. For an
ARM-only `MH_BUNDLE`, hao supplies a resident guest handle after the native open
fails. The guest loader validates the descriptor it maps, binds imports,
registers metadata and classes and runs constructors. Native class-to-image
lookup reports the guest image path, so `classNamed:` and `bundleForClass:`
keep the bundle's resource identity. Native and universal bundles stay with
dyld.

Guest handles support data and class lookup, `RTLD_FIRST`, and local and
global data visibility. Native symbol definitions keep precedence. Untyped C
function exports are refused: a `dlsym` name alone does not describe an
aggregate or callback ABI. An ARM-only dylib (a framework's executable) falls
back the same way. Guest images stay resident after close, because registered classes
and escaping callbacks can still reference them.

The test compares native ARM execution with JIT, interpreter and verified AOT
execution of both the program and the guest bundle: constructor-once
behaviour, class lookup, resources, repeated loads and closes, mixed native and
guest error state, global data, untyped function refusal and unchanged
universal behaviour. Corrupt signed ARM pages must be refused by the loader or
killed by page-signature enforcement, and a universal fixture with an invalid
native slice and an intact ARM slice must not fall back to ARM.

The class-to-image lookup is part of `libhao_identity.dylib`, so a deployment
updates both runtime libraries and the matching AOT caches together.
