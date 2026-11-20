import SRLib
struct Word: Source { let value: UInt32 }
struct Count: Source { let value: Int }
struct Ratio: Source { var value: Double { 2.5 } }
for w in [UInt32(0), 7, 0xdeadbeef, .max] { print("UInt32", describe(Word(value: w)), pair(Word(value: w))) }
for c in [0, -1, Int.min, Int.max] { print("Int", describe(Count(value: c)), pair(Count(value: c))) }
print("Double", describe(Ratio()), pair(Ratio()))
