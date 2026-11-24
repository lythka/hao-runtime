import XCTest
import BoundaryPackage
final class DependencyTests: XCTestCase {
 func testPublicDependency() throws {
  XCTAssertEqual(try checked(12), 144)
  XCTAssertEqual(try Envelope(["local", "世界"]).roundTrip(), ["local", "世界"])
 }
}
