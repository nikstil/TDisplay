// swift-tools-version:5.9
// The step 1 feasibility tool from mac/PLAN.md: a virtual display, ScreenCaptureKit capture of it and VideoToolbox
// encoding, each measured on its own. Build with ./build.sh, which also wraps it in an .app for the Screen Recording
// permission.
import PackageDescription

let package = Package(
    name: "TDisplayFeasibility",
    platforms: [.macOS("26.0")],
    targets: [
        .target(
            name: "VirtualDisplayShim",
            path: "Sources/VirtualDisplayShim",
            linkerSettings: [.linkedFramework("CoreGraphics")]),
        .executableTarget(
            name: "tdisplay-feasibility",
            dependencies: ["VirtualDisplayShim"],
            path: "Sources/tdisplay-feasibility",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("Accelerate"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("QuartzCore"),
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("VideoToolbox"),
            ]),
    ]
)
