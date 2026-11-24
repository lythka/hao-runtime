import XCTest
@testable import BoundaryPackage
final class FixtureTests: XCTestCase {
 func testGenericCodable() throws { XCTAssertEqual(try Envelope(["hello", "世界", "🙂"]).roundTrip(), ["hello", "世界", "🙂"]) }
 func testErrorAndResult() throws { XCTAssertThrowsError(try checked(-1)); XCTAssertEqual(try checked(12), 144) }
 func testActorAndTasks() async {
  let counter = Counter()
  let a = await counter.add(7)
  let b = await counter.add(5)
  let sum = await parallelSum()
  XCTAssertEqual(a, 7); XCTAssertEqual(b, 12); XCTAssertEqual(sum, 5050)
 }
}
