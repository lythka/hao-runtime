# Value contracts

`optional-data-26B131.json` is version `0.1.0-26B131.1` of the bounded
`URLRequest.httpBody` nil-argument adapter for macOS 27.2.1 (26B131). It binds
the ARM and Intel Foundation cache/image UUIDs, the setter export and four
reviewed code regions on each architecture. It preserves live Data words and
the receiver; it does not cover the getter, results or the reverse conversion.

The setter and release helper still distinguish ARM high-nibble 0xf from Intel
high-byte 0xff. CoW mutation forwards both value words unchanged. The getter
constructs those nil words itself. The records hold identities and hashes only.

Checked: both original caches match every recorded anchor; the compiler layout
check and packaging pass; 14 paired native ARM/Intel CLI references agree.
On Intel hardware (MacBookPro9,2, 26B131) the 14 cases pass under JIT,
interpreter and AOT, and native Intel matches native ARM. A raw nil fails in every translated mode, and without the adapter the call is
refused before native entry. Not covered: installed images and applications.
