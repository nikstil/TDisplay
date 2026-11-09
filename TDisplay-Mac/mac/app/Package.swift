// swift-tools-version:5.9
// The TDisplay menu bar app for macOS: the counterpart of host/ on Windows. Build with ./build.sh, which also makes the
// .app bundle.
import PackageDescription

let package = Package(
    name: "TDisplay",
    platforms: [.macOS("15.0")],
    targets: [
        .target(
            name: "VirtualDisplayShim",
            path: "Sources/VirtualDisplayShim",
            linkerSettings: [.linkedFramework("CoreGraphics")]),
        .executableTarget(
            name: "TDisplay",
            dependencies: ["VirtualDisplayShim"],
            path: "Sources/TDisplay",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("IOKit"),
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("ServiceManagement"),
                .linkedFramework("VideoToolbox"),
            ]),
    ]
)
