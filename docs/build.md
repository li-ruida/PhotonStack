# Build and Packaging

PhotonStack currently targets macOS first.

## Requirements

Install the following on macOS:

- Xcode with the macOS SDK
- Homebrew
- CMake 3.20 or later

`clang-format` is required only when running the formatting helper.

Install repository-managed dependencies with:

```bash
brew bundle
```

## Standard Build

Preferred build flow:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Equivalent helper scripts:

```bash
tools/scripts/build.sh debug
tools/scripts/test.sh debug
```

## Script Reference

- `tools/scripts/build.sh`: configure and build the CMake targets
- `tools/scripts/test.sh`: configure, build, and run the test suite
- `tools/scripts/format.sh`: apply the repository C++ formatting rules
- `tools/scripts/build-macos.sh`: build the SwiftPM macOS app target
- `tools/scripts/package-macos.sh`: package the macOS app bundle and embedded CLI
- `tools/scripts/run-macos-dev.sh`: local helper for launching, debugging, or log-streaming the packaged app
- `tools/scripts/test-raw-smoke.sh`: RAW workflow smoke test for a local sample file

## macOS App Packaging

Build and package the app with:

```bash
tools/scripts/build.sh debug
tools/scripts/build-macos.sh
tools/scripts/package-macos.sh
```

The packaged app is written to:

```text
build/PhotonStackMac.app
```

`tools/scripts/package-macos.sh` currently:

- builds the CLI and macOS app targets
- creates `build/PhotonStackMac.app`
- embeds the `photonstack` CLI in the app bundle
- writes bundle metadata and build identity files
- generates `AppIcon.icns` from the repository PNG source when Python and Pillow are available
- applies ad hoc signing when `codesign` is available

## Local App Development

Helper examples:

```bash
tools/scripts/run-macos-dev.sh run
tools/scripts/run-macos-dev.sh debug
tools/scripts/run-macos-dev.sh logs
tools/scripts/run-macos-dev.sh telemetry
tools/scripts/run-macos-dev.sh verify
```

These modes package the app first, then launch or inspect the packaged build.

## CLI Examples

```bash
build/debug/apps/PhotonStackCLI/photonstack --version
build/debug/apps/PhotonStackCLI/photonstack inspect path/to/image.png
build/debug/apps/PhotonStackCLI/photonstack preview --input path/to/image.png --output preview.png --width 1600
build/debug/apps/PhotonStackCLI/photonstack stack --output result.tiff --method average image-a.png image-b.png
build/debug/apps/PhotonStackCLI/photonstack calibrate --light light.nef --output calibrated.fits --dark master-dark.fits
PHOTONSTACK_PROGRESS=1 build/debug/apps/PhotonStackCLI/photonstack run workflow.psflow
```

## Notes

- `build/` and `.build/` are local build outputs and are intentionally ignored by Git.
- The repository is macOS-first; some workflows depend on Apple imaging support.
- For the current public repository, `README.md` stays intentionally short and this document is the main build reference.
