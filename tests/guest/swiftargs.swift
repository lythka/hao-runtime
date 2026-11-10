import Foundation
let args = CommandLine.arguments
print("count", args.count)
for a in args.dropFirst() { print("arg", a, a.utf8.count, a == "-lint", a == "-p") }
let pi = ProcessInfo.processInfo.arguments
print("pi", pi.dropFirst())
switch args.count > 1 ? args[1] : "" {
case "-lint": print("matched lint")
case "-p": print("matched p")
default: print("no match")
}
var d: [String: Int] = [:]
for (i, w) in ["alpha", "beta", "gamma-delta-epsilon-long"].enumerated() { d[w] = i }
print(d.keys.sorted(), d["beta"] ?? -1)
print(String(format: "%05.1f|%@", 3.14159, "objc"))
