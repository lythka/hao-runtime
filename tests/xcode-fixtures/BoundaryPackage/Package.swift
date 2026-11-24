// swift-tools-version: 6.0
import PackageDescription
let package = Package(name: "BoundaryPackage", platforms: [.macOS(.v13)], products: [.library(name: "BoundaryPackage", targets: ["BoundaryPackage"])], targets: [.target(name: "BoundaryPackage"), .testTarget(name: "BoundaryPackageTests", dependencies: ["BoundaryPackage"])])
