import Testing
@testable import BoundaryPackage
@Test("Codable generic round trip") func coding() throws {
 #expect(try Envelope([1, 2, 3]).roundTrip() == [1, 2, 3])
}
@Test(arguments: [0, 1, 12]) func squares(_ value: Int) throws {
 #expect(try checked(value) == value * value)
}
@Test("Concurrent task group result") func tasks() async {
 #expect(await parallelSum() == 5050)
}
