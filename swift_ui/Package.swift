// swift-tools-version: 5.10
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "swift_ui",
    platforms: [
        .macOS(.v14)
    ],
    dependencies: [
        .package(url: "https://github.com/grpc/grpc-swift.git", from: "1.23.0"),
        .package(url: "https://github.com/apple/swift-protobuf.git", from: "1.25.2")
    ],
    targets: [
        .executableTarget(
            name: "VURIO",
            dependencies: [
                .product(name: "GRPC", package: "grpc-swift"),
                .product(name: "SwiftProtobuf", package: "swift-protobuf")
            ],
            path: "Sources/swift_ui"
        ),
        .testTarget(
            name: "swift_uiTests",
            dependencies: ["VURIO"]),
    ]
)
