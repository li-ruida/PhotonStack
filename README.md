# PhotonStack

PhotonStack is a macOS-first astrophotography processing project with a shared
C++ engine, a desktop app, and a command-line interface.

## Overview

The public repository currently includes:

- a shared C++20 processing engine
- a macOS desktop application
- a CLI for automation and reproducible workflows
- build scripts, tests, and packaging helpers

Current processing coverage includes:

- inspect, convert, preview, and workflow execution
- calibration, registration, stacking, drizzle, and mosaic paths
- histogram, quality, normalize, stretch, curves, contrast, denoise, sharpen,
  deconvolution, and color adjustments
- star detection, star masks, and star reduction
- FITS-oriented commands
- Apple-supported RAW / ProRAW workflows on macOS

## Documentation

For the full documentation guide, start with [`docs/README.md`](docs/README.md).

## License

PhotonStack is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE)
for details.
