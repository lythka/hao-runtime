import Observation

@Observable final class Model {
    var value = 0
}
final class Counter: @unchecked Sendable {
    var value = 0
}
@inline(never) func subscribe(_ model: Model, _ counter: Counter, _ increment: Int) {
    withObservationTracking {
        _ = model.value
    } onChange: {
        counter.value += increment
    }
}
let model = Model()
let counter = Counter()
for i in 1...4 {
    subscribe(model, counter, i)
    model.value += 1
}
print("observation", model.value, counter.value)
// Only onChange's factory result is a known closure. The apply callback is
// generic and may return arbitrary data, including wide indirect results.
let wide = withObservationTracking {
    (model.value, 2, 3, 4, 5)
} onChange: { counter.value += 20 }
model.value += 1
print("generic result", wide.0, wide.1, wide.2, wide.3, wide.4, counter.value)
