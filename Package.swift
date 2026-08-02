// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "PhotonStack",
    platforms: [
        .macOS(.v14),
        .iOS(.v17),
    ],
    products: [
        .library(name: "PhotonStackAppCore", targets: ["PhotonStackAppCore"]),
        .library(name: "PhotonStackEngineBridge", targets: ["PhotonStackEngineBridge"]),
        .library(name: "PhotonStackProcessing", targets: ["PhotonStackProcessing"]),
        .library(name: "PhotonStackUI", targets: ["PhotonStackUI"]),
        .executable(name: "PhotonStackMac", targets: ["PhotonStackMac"]),
    ],
    targets: [
        .target(
            name: "PhotonStackAppCore",
            path: "packages/PhotonStackAppCore/Sources"
        ),
        .testTarget(
            name: "PhotonStackAppCoreTests",
            dependencies: ["PhotonStackAppCore"],
            path: "packages/PhotonStackAppCore/Tests"
        ),
        .target(
            name: "PhotonStackEngineBridge",
            path: "engine",
            sources: [
                "src/ArtifactTrailRemover.cpp",
                "src/BackgroundExtractor.cpp",
                "src/Calibrator.cpp",
                "src/CloudRemoval.cpp",
                "src/ComaReducer.cpp",
                "src/ColorAdjuster.cpp",
                "src/Curves.cpp",
                "src/Deconvolution.cpp",
                "src/DrizzleStacker.cpp",
                "src/FitsCodec.cpp",
                "src/FrameQualityAnalyzer.cpp",
                "src/FrameNormalizer.cpp",
                "src/Histogram.cpp",
                "src/ImageCodec.cpp",
                "src/ImageInspector.cpp",
                "src/ImageResizer.cpp",
                "src/LocalContrast.cpp",
                "src/RawCodec.cpp",
                "src/Registration.cpp",
                "src/ScientificDisplayBridge.cpp",
                "src/Stacker.cpp",
                "src/StarDetector.cpp",
                "src/StarMask.cpp",
                "src/StarReducer.cpp",
                "src/Stretch.cpp",
                "src/MasterFrameBuilder.cpp",
                "src/MeteorLayerComposer.cpp",
                "src/MosaicBuilder.cpp",
                "src/NoiseReducer.cpp",
                "src/PhotonStackBridge.cpp",
                "src/Sharpen.cpp",
                "src/TileProcessor.cpp",
                "src/AppleRawDecoder.mm",
            ],
            publicHeadersPath: "bridge/include",
            cSettings: [
                .headerSearchPath("include"),
            ],
            cxxSettings: [
                .headerSearchPath("include"),
            ],
            linkerSettings: [
                .linkedFramework("CoreFoundation"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("CoreImage"),
                .linkedFramework("Foundation"),
                .linkedFramework("ImageIO"),
                .linkedFramework("Metal"),
            ]
        ),
        .target(
            name: "PhotonStackProcessing",
            dependencies: [
                "PhotonStackAppCore",
                "PhotonStackEngineBridge",
            ],
            path: "packages/PhotonStackProcessing/Sources"
        ),
        .testTarget(
            name: "PhotonStackProcessingTests",
            dependencies: ["PhotonStackProcessing"],
            path: "packages/PhotonStackProcessing/Tests"
        ),
        .target(
            name: "PhotonStackUI",
            dependencies: [
                "PhotonStackAppCore",
                "PhotonStackProcessing",
            ],
            path: "packages/PhotonStackUI/Sources"
        ),
        .testTarget(
            name: "PhotonStackUITests",
            dependencies: ["PhotonStackUI"],
            path: "packages/PhotonStackUI/Tests"
        ),
        .executableTarget(
            name: "PhotonStackMac",
            dependencies: [
                "PhotonStackAppCore",
                "PhotonStackProcessing",
                "PhotonStackUI",
            ],
            path: "apps/PhotonStackMac/Sources"
        ),
    ],
    cxxLanguageStandard: .cxx20
)
