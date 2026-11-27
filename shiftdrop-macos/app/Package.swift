// swift-tools-version:5.9
import PackageDescription

// libwebp is built from source by ../scripts/build-libwebp-macos.sh into vendor/libwebp (ImageIO can't
// write WebP). Run ../scripts/build-engines.sh once before `swift build`.
let webp = Context.packageDirectory + "/vendor/libwebp"

let package = Package(
    name: "ShiftDrop",
    platforms: [.macOS("13.4")],
    targets: [
        .systemLibrary(name: "CWebP", path: "Sources/CWebP"),
        .executableTarget(
            name: "ShiftDrop",
            dependencies: ["CWebP"],
            path: "Sources/ShiftDrop",
            swiftSettings: [.unsafeFlags(["-Xcc", "-I\(webp)/include"])],
            linkerSettings: [
                // Linked statically: the app needs no libwebp on the Mac it runs on.
                .unsafeFlags(["\(webp)/lib/libwebp.a", "\(webp)/lib/libsharpyuv.a"]),
            ]
        ),
        // Unit tests (Swift Testing) of the pure parts (ffmpeg log parsing, size planning, subtitles, sheets, output names).
        // `swift test` needs the engines built once (the package links libwebp), same as `swift build`.
        .testTarget(name: "ShiftDropTests", dependencies: ["ShiftDrop"], path: "Tests/ShiftDropTests",
                    swiftSettings: [.unsafeFlags(["-Xcc", "-I\(webp)/include"])]),
    ]
)
