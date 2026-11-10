import AssociatedNative

extension Associated {
    @inline(never) func packed(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ e: Int, _ f: Int,
                              _ g: Int, _ h: Int, _ i: Int8, _ j: Int16) -> Int {
        a + b + c + d + e + f + g + h + Int(i) * 100 + Int(j) * 10000
    }
}
struct ResultBox<T> { let value: T; let marker: Int }
struct Guest<T>: Associated {
    let value: T
    @inline(never) var first: T { value }
    @inline(never) var second: ResultBox<T> { ResultBox(value: value, marker: 81) }
}
struct Twin: Associated {
    @inline(never) var first: Wide {
        // A native-to-guest callback nested inside another getter.
        invoke(Guest(value: 91))
        return Wide(101)
    }
    @inline(never) var second: Int { 111 }
}
invoke(Guest(value: 71))
invoke(Guest(value: (72, 73)))
invoke(Twin())
