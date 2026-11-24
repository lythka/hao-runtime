import SwiftUI
import Foundation
@main struct HaoStressApp: App {
 var body: some Scene { WindowGroup { DemoView() } }
}
struct DemoView: View {
 @State private var result = "Building a report…"
 var body: some View {
  NavigationStack { List { Text("Compiled on Intel through hao").font(.headline); Text(result).monospaced() }.navigationTitle("Compiler coverage") }
   .task { result = await report() }
 }
 func report() async -> String {
  let values = [Worker0.compute(3), Worker1.compute(3), Worker2.compute(3), Worker4.compute(3), Worker5.compute(3), Worker6.compute(3), Worker7.compute(3)]
  let concurrent = await Worker3.run()
  return "Swift: \(values.reduce(0,+))\nTasks: \(concurrent)\nObjective-C++: \(demoNativeChecksum(values.map { NSNumber(value:$0) }))"
 }
}
