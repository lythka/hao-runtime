# Named variadic Objective-C method contracts

```sh
python3.14 tests/guest/objcvascope.py
```

Builds a native universal helper and native ARM and guest callers, and compares
native ARM with JIT, interpreter and AOT, checking that the AOT cache actually
loads. The optional `HAO_BASE` names a runtime without the fix, which must fail
the collision cases while passing the controls. `OUT` chooses a separate output
folder; each run has a 60-second timeout. No GUI or private framework is used.

## What it tests

hao has named variadic recipes for Foundation methods such as
`stringWithFormat:`. Native classes outside that family can declare methods
with the same selectors and different meaning, for example
`-[NSDate localizedStringWithFormat:]` in a framework category. A recipe chosen
by selector alone would apply NSString's calling convention, or send `alloc`,
to the wrong class.

The fixture's instance and unrelated class methods share NSString and
collection selectors. An unrelated subclass override, a guest superclass call
(`objc_msgSendSuper2`), raw `objc_msgSendSuper` and a native NSString subclass
override must keep their own results. NSString's mixed object, integer and
double formats, inherited NSMutableString class formatting, the
NSArray/NSSet/NSOrderedSet/NSDictionary class-cluster initializers and mutable
string appends must still match native.

## How hao handles it

`src/objc.c` requires the declaring family and method kind from the SDK before
using a named recipe, then compares the selected IMP with that family's
implementation. Inherited methods keep the recipe. Native overrides keep
ordinary dispatch, the sibling rule and machine-code variadic inference. There
is no application, address or UUID rule.

The declarations come from the Foundation SDK headers NSString.h
(`stringWithFormat:`, `localizedStringWithFormat:`, `appendFormat:`,
`stringByAppendingFormat:`), NSArray.h, NSSet.h, NSOrderedSet.h,
NSDictionary.h and NSException.h (`NSAssertionHandler`), and AppKit's NSAlert.h.
`tests/guest/objcva.sh` covers the recipes themselves, including superclass
dispatch.
