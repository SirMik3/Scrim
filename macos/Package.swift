// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "Scrim",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "Scrim",
            path: "Sources/Scrim",
            linkerSettings: [
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("IOKit"),
                .linkedFramework("ServiceManagement"),
            ]
        )
    ]
)
