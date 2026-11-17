// W^X with Swift metadata (tests/guest/wx.sh): the program's conformances and closures get island entries;
// after they have run, no memory is writable and executable at once (wx_regions: tests/guest/wx.c).
@_silgen_name("wx_regions") func wx_regions() -> Int32

protocol Shape { func area() -> Double }
struct Square: Shape { var s: Double; func area() -> Double { s * s } }
struct Circle: Shape { var r: Double; func area() -> Double { 3.0 * r * r } }

@main struct Main {
    static func main() {
        var shapes: [any Shape] = []
        for i in 0..<200 {
            let d = Double(i)
            if i % 2 == 0 { shapes.append(Square(s: d)) } else { shapes.append(Circle(r: d.truncatingRemainder(dividingBy: 17))) }
        }
        let sorted = shapes.sorted { $0.area() < $1.area() }
        print("smallest \(sorted[0].area()) largest \(sorted[sorted.count - 1].area())")
        print("writable and executable regions: \(wx_regions())")
    }
}
