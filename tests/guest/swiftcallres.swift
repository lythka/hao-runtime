import SCLib
// tests/guest/swiftcallres.sh: native Swift calls whose direct values differ in their nil's encoding.
@main struct Main {
    static func main() {
        let c = Card(0)
        for k in 0..<6 {
            let v = Card(k).labels   // a result in x0-x3
            if let t = v { print("get", k, t.title ?? "nil", t.subtitle ?? "nil") } else { print("get", k, "none") }
            print("show", k, show(v))   // an argument in x0-x3
            c.labels = v                // a setter's argument
            print("set", k, show(c.labels))
        }
    }
}
