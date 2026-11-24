// Emit in bit-pattern order so a mismatch identifies the exact input.
@inline(never)
func describe(_ bits: UInt16) -> String {
    Float16(bitPattern: bits).description
}

for raw in UInt16.min...UInt16.max {
    print(describe(raw))
}
