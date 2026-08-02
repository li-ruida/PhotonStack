# Architecture

PhotonStack is a macOS-first astrophotography application with a shared C++
processing engine and a command-line interface.

## Repository layout

```text
apps/
  PhotonStackCLI/       C++ command-line executable
  PhotonStackMac/       SwiftUI macOS application entry point
engine/
  include/photonstack/  Public C++ engine headers
  src/                  C++20 engine implementation and Apple RAW decoder
packages/
  PhotonStackAppCore/   Project, settings, and workspace models
  PhotonStackProcessing/ Swift-to-engine processing services
  PhotonStackUI/        SwiftUI views and native macOS integrations
tests/                  C++ engine and CLI tests
tools/scripts/          Build, test, formatting, and packaging helpers
```

## Runtime structure

```text
SwiftUI macOS app                  C++ CLI
        |                              |
        +-- Swift processing services --+
                        |
                  C++20 engine
                        |
 CoreFoundation / CoreGraphics / CoreImage / Foundation / ImageIO
```

The macOS app keeps project state, UI behavior, and file-panel integration in
Swift packages. Image-processing operations are implemented by the shared C++
engine and exposed to the app through the repository's bridge and CLI-facing
services. The CLI links the same engine directly.

## Build structure

The C++ engine and CLI use CMake presets. The macOS application is organized as
a Swift package. On Apple platforms, the engine builds its Objective-C++ RAW
decoder and links the Apple imaging frameworks declared in
[`engine/CMakeLists.txt`](../engine/CMakeLists.txt).

For build and packaging commands, see [`build.md`](build.md). For the current
user-facing capabilities, see [`features.md`](features.md).
