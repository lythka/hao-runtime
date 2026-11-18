// Swift storage whose element type holds a native Optional with a nil encoded differently on arm64 and
// x86_64 (Data?, Obj??), made by guest code and destroyed last by native code (tests/guest/carrierdestroy.sh):
// arrays held by a native NSMutableArray past the guest's last reference and generic boxes that native
// dispatch blocks capture. The structs use layout-string value witnesses; the runtime builds the layout
// strings of those holding a UUID, and then reads their fields' nils natively. Big has 241 stored fields.
// Output must equal native arm64.
import Foundation

final class Obj { let n: Int; init(_ n: Int) { self.n = n }; deinit { print("deinit obj\(n)") } }
let id = UUID(uuidString: "00000000-0000-0000-0000-000000000001")!
struct Small { var d: Data?; var o: Obj??; var n: Int }
struct Laid { var d: Data?; var o: Obj??; var u = id; var n: Int }
struct Big {
    var d: Data?
    var u = id
    var f1: Int = 1
    var f2: Int = 2
    var f3: Int = 3
    var f4: Int = 4
    var f5: Int = 5
    var f6: Int = 6
    var f7: Int = 7
    var f8: Int = 8
    var f9: Int = 9
    var f10: Int = 10
    var f11: Int = 11
    var f12: Int = 12
    var f13: Int = 13
    var f14: Int = 14
    var f15: Int = 15
    var f16: Int = 16
    var f17: Int = 17
    var f18: Int = 18
    var f19: Int = 19
    var f20: Int = 20
    var f21: Int = 21
    var f22: Int = 22
    var f23: Int = 23
    var f24: Int = 24
    var f25: Int = 25
    var f26: Int = 26
    var f27: Int = 27
    var f28: Int = 28
    var f29: Int = 29
    var f30: Int = 30
    var f31: Int = 31
    var f32: Int = 32
    var f33: Int = 33
    var f34: Int = 34
    var f35: Int = 35
    var f36: Int = 36
    var f37: Int = 37
    var f38: Int = 38
    var f39: Int = 39
    var f40: Int = 40
    var f41: Int = 41
    var f42: Int = 42
    var f43: Int = 43
    var f44: Int = 44
    var f45: Int = 45
    var f46: Int = 46
    var f47: Int = 47
    var f48: Int = 48
    var f49: Int = 49
    var f50: Int = 50
    var f51: Int = 51
    var f52: Int = 52
    var f53: Int = 53
    var f54: Int = 54
    var f55: Int = 55
    var f56: Int = 56
    var f57: Int = 57
    var f58: Int = 58
    var f59: Int = 59
    var f60: Int = 60
    var f61: Int = 61
    var f62: Int = 62
    var f63: Int = 63
    var f64: Int = 64
    var f65: Int = 65
    var f66: Int = 66
    var f67: Int = 67
    var f68: Int = 68
    var f69: Int = 69
    var f70: Int = 70
    var f71: Int = 71
    var f72: Int = 72
    var f73: Int = 73
    var f74: Int = 74
    var f75: Int = 75
    var f76: Int = 76
    var f77: Int = 77
    var f78: Int = 78
    var f79: Int = 79
    var f80: Int = 80
    var f81: Int = 81
    var f82: Int = 82
    var f83: Int = 83
    var f84: Int = 84
    var f85: Int = 85
    var f86: Int = 86
    var f87: Int = 87
    var f88: Int = 88
    var f89: Int = 89
    var f90: Int = 90
    var f91: Int = 91
    var f92: Int = 92
    var f93: Int = 93
    var f94: Int = 94
    var f95: Int = 95
    var f96: Int = 96
    var f97: Int = 97
    var f98: Int = 98
    var f99: Int = 99
    var f100: Int = 100
    var f101: Int = 101
    var f102: Int = 102
    var f103: Int = 103
    var f104: Int = 104
    var f105: Int = 105
    var f106: Int = 106
    var f107: Int = 107
    var f108: Int = 108
    var f109: Int = 109
    var f110: Int = 110
    var f111: Int = 111
    var f112: Int = 112
    var f113: Int = 113
    var f114: Int = 114
    var f115: Int = 115
    var f116: Int = 116
    var f117: Int = 117
    var f118: Int = 118
    var f119: Int = 119
    var f120: Int = 120
    var f121: Int = 121
    var f122: Int = 122
    var f123: Int = 123
    var f124: Int = 124
    var f125: Int = 125
    var f126: Int = 126
    var f127: Int = 127
    var f128: Int = 128
    var f129: Int = 129
    var f130: Int = 130
    var f131: Int = 131
    var f132: Int = 132
    var f133: Int = 133
    var f134: Int = 134
    var f135: Int = 135
    var f136: Int = 136
    var f137: Int = 137
    var f138: Int = 138
    var f139: Int = 139
    var f140: Int = 140
    var f141: Int = 141
    var f142: Int = 142
    var f143: Int = 143
    var f144: Int = 144
    var f145: Int = 145
    var f146: Int = 146
    var f147: Int = 147
    var f148: Int = 148
    var f149: Int = 149
    var f150: Int = 150
    var f151: Int = 151
    var f152: Int = 152
    var f153: Int = 153
    var f154: Int = 154
    var f155: Int = 155
    var f156: Int = 156
    var f157: Int = 157
    var f158: Int = 158
    var f159: Int = 159
    var f160: Int = 160
    var f161: Int = 161
    var f162: Int = 162
    var f163: Int = 163
    var f164: Int = 164
    var f165: Int = 165
    var f166: Int = 166
    var f167: Int = 167
    var f168: Int = 168
    var f169: Int = 169
    var f170: Int = 170
    var f171: Int = 171
    var f172: Int = 172
    var f173: Int = 173
    var f174: Int = 174
    var f175: Int = 175
    var f176: Int = 176
    var f177: Int = 177
    var f178: Int = 178
    var f179: Int = 179
    var f180: Int = 180
    var f181: Int = 181
    var f182: Int = 182
    var f183: Int = 183
    var f184: Int = 184
    var f185: Int = 185
    var f186: Int = 186
    var f187: Int = 187
    var f188: Int = 188
    var f189: Int = 189
    var f190: Int = 190
    var f191: Int = 191
    var f192: Int = 192
    var f193: Int = 193
    var f194: Int = 194
    var f195: Int = 195
    var f196: Int = 196
    var f197: Int = 197
    var f198: Int = 198
    var f199: Int = 199
    var f200: Int = 200
    var f201: Int = 201
    var f202: Int = 202
    var f203: Int = 203
    var f204: Int = 204
    var f205: Int = 205
    var f206: Int = 206
    var f207: Int = 207
    var f208: Int = 208
    var f209: Int = 209
    var f210: Int = 210
    var f211: Int = 211
    var f212: Int = 212
    var f213: Int = 213
    var f214: Int = 214
    var f215: Int = 215
    var f216: Int = 216
    var f217: Int = 217
    var f218: Int = 218
    var f219: Int = 219
    var f220: Int = 220
    var f221: Int = 221
    var f222: Int = 222
    var f223: Int = 223
    var f224: Int = 224
    var f225: Int = 225
    var f226: Int = 226
    var f227: Int = 227
    var f228: Int = 228
    var f229: Int = 229
    var f230: Int = 230
    var f231: Int = 231
    var f232: Int = 232
    var f233: Int = 233
    var f234: Int = 234
    var f235: Int = 235
    var f236: Int = 236
    var f237: Int = 237
    var f238: Int = 238
    var f239: Int = 239
}
let held = NSMutableArray()

@inline(never) func hold(_ v: Any) { held.add(v) }
@inline(never) func drop(_ what: String) { print(what, "held", held.count); held.removeAllObjects() }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func boxed<T>(_ v: T, _ show: @escaping (T) -> String) {
    var p = v
    let g = DispatchGroup()
    DispatchQueue.global().async(group: g) { print("box", show(p)); p = v }
    g.wait()
    DispatchQueue.global().async(group: g) { print("box again", show(p)) }
    g.wait()
}

func describe(_ d: Data?, _ o: Obj??, _ n: Int) -> String {
    "\(d?.count ?? -1) \(o.map { $0.map { "obj\($0.n)" } ?? "inner-nil" } ?? "nil") \(n)"
}
func describe(_ s: Small) -> String { describe(s.d, s.o, s.n) }
func describe(_ s: Laid) -> String { describe(s.d, s.o, s.n) + " \(s.u == id)" }

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        var datas: [Data?] = [nil, Data([1, 2, 3]), nil]
        datas.append(nil)
        print("datas", datas.count, datas.filter { $0 == nil }.count)
        hold(datas); datas = []; drop("datas")
        var objs: [Obj??] = [.none, .some(nil), .some(Obj(1))]
        objs.append(.none)
        print("objs", objs.count, objs.filter { $0 == nil }.count)
        hold(objs); objs = []; drop("objs")
        var smalls = [Small(d: nil, o: .none, n: 1), Small(d: Data([9]), o: .some(nil), n: 2),
                      Small(d: nil, o: .some(Obj(3)), n: 3)]
        smalls.append(Small(d: nil, o: nil, n: 4))
        print("smalls", smalls.map(describe).joined(separator: ", "))
        hold(smalls); smalls = []; drop("smalls")
        var laids = [Laid(d: nil, o: .none, n: 1), Laid(d: Data([9]), o: .some(nil), n: 2),
                     Laid(d: nil, o: .some(Obj(4)), n: 3)]
        laids.append(Laid(d: nil, o: nil, n: 4))
        print("laids", laids.map(describe).joined(separator: ", "))
        hold(laids); laids = []; drop("laids")
        var bigs = [Big(), Big(d: Data([7, 7]))]
        bigs.append(Big())
        print("bigs", bigs.count, bigs.filter { $0.d == nil }.count, bigs[2].f239)
        hold(bigs); bigs = []; drop("bigs")
        boxed(Small(d: nil, o: .none, n: 5), describe)
        boxed(Laid(d: nil, o: .none, n: 6), describe)
        boxed(Big()) { "\($0.d == nil) \($0.f100)" }
        print("done")
    }
}
