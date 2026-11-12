import Foundation
import ODLib
let four = Data([1, 2, 3, 4])
print("sig nil", odSignature(1, 2, nil, sig: nil))
print("sig 4", odSignature(1, 2, [3], sig: four))
print("sig empty", odSignature(1, 2, nil, sig: Data()))
print("sig 20", odSignature(1, 2, nil, sig: Data(count: 20)))
print("two", odTwo(nil, 3, four), odTwo(four, 3, nil), odTwo(nil, 0, nil), odTwo(Data(), 1, Data(count: 30)))
for n in [-1, 0, 3, 15, 40] { let r = odMaybe(n); print("maybe", n, r == nil ? "nil" : "\(r!.count)") }
