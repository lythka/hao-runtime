// A guest single-payload enum whose payload is a tuple (tests/guest/svtuplexi.sh). The tuple holds a
// resilient native struct, so its metadata and tag witnesses are the runtime's: the enum's empty cases are
// stored in the tuple's class reference in the native encoding (x86_64: extra inhabitant k is the word 2k),
// as Weather's WeatherData holds one. Guest generic code hands the enclosing struct, its Optional and an
// Array of them to native value witnesses.
import Foundation

final class Station { let name: String; init(_ n: String) { name = n } }
enum Reading { case taken((Date, Station, String)); case pending }
enum Forecast { case ready((Date, Station, Int)); case stale, missing, offline }
struct Report { var id: Int; var reading: Reading; var forecast: Forecast; var note: String? }

@_semantics("optimize.sil.specialize.generic.never") @inline(never)
func copied<T>(_ v: T) -> T { let w = v; return w }

func shown(_ r: Reading) -> String {
    switch r { case .taken(let t): return "taken(\(t.0.timeIntervalSince1970),\(t.1.name),\(t.2))"; case .pending: return "pending" }
}
func shown(_ f: Forecast) -> String {
    switch f { case .ready(let t): return "ready(\(t.0.timeIntervalSince1970),\(t.1.name),\(t.2))"
    case .stale: return "stale"; case .missing: return "missing"; case .offline: return "offline" }
}
func shown(_ r: Report) -> String { "\(r.id) \(shown(r.reading)) \(shown(r.forecast)) \(r.note ?? "nil")" }

@main struct Main {
    static func main() {
        setbuf(stdout, nil)
        let s = Station("s")
        let readings: [Reading] = [.taken((Date(timeIntervalSince1970: 7), s, "r")), .pending]
        let forecasts: [Forecast] = [.ready((Date(timeIntervalSince1970: 9), s, 3)), .stale, .missing, .offline]
        var all: [Report] = []
        for (i, r) in readings.enumerated() {
            for (j, f) in forecasts.enumerated() {
                let rep = Report(id: 10 * i + j, reading: r, forecast: f, note: j == 1 ? "n" : nil)
                all.append(rep)
                print(shown(copied(rep)), copied(Optional(rep)).map(shown) ?? "nil", copied(Report?.none) == nil)
                print(shown(copied(r)), copied(Optional(f)).map(shown) ?? "nil")
            }
        }
        print(copied(all).map(shown))
    }
}
