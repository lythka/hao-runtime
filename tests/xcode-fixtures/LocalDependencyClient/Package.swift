// swift-tools-version: 6.0
import PackageDescription
let package = Package(name: "LocalDependencyClient", platforms: [.macOS(.v13)], dependencies: [.package(path: "../BoundaryPackage")], targets: [.testTarget(name: "LocalDependencyClientTests", dependencies: [.product(name: "BoundaryPackage", package: "BoundaryPackage")])])
