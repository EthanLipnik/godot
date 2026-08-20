// swift-tools-version: 6.2

import PackageDescription

let package = Package(
	name: "RemoteXRCompanionCore",
	platforms: [.macOS(.v13)],
	products: [.library(name: "RemoteXRCompanionCore", targets: ["RemoteXRCompanionCore"])],
	targets: [
		.target(name: "RemoteXRCompanionCore"),
		.testTarget(name: "RemoteXRCompanionCoreTests", dependencies: ["RemoteXRCompanionCore"]),
	]
)
