# Reviewed value contracts

`optional-data-26B131.json` is version `0.1.0-26B131.1` of the bounded
`URLRequest.httpBody` nil-argument adapter for Golden Gate 27.2.1. It binds the
ARM and Intel Foundation cache/image UUIDs, setter export and four reviewed
code regions on each architecture. It preserves live Data words and the
receiver; it does not authorize getter/result or reverse conversion.

The setter and release helper still distinguish ARM high-nibble 0xf from Intel
high-byte 0xff. CoW mutation forwards both value words unchanged. The getter
independently constructs those nil words. Public records contain identities
and hashes only; original disassembly remains private research evidence.

Validation: both original caches match all recorded anchors; compiler layout
proof and packaging pass; 14 paired native ARM/Intel CLI references agree.
On a physical MacBookPro9,2 running 26B131, all 14 cases pass under JIT,
interpreter and verified AOT (42 translated cases), with 14 native Intel controls
matching native ARM. Raw nil fails in every translated mode; removing the
adapter refuses before native entry. The exported own-code library reproduces
byte for byte from committed generator sources. Installed-image and application
validation remain pending; this version does not claim production acceptance.
