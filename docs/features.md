# Current Capabilities

This document summarizes what the current public PhotonStack repository can do
today and where the main boundaries still are.

It is intentionally higher level than an internal validation log or a detailed
engineering checklist.

## Repository Scope

The repository currently contains:

- a shared C++20 processing engine
- a macOS-first desktop application
- a command-line interface for automation and reproducible workflows
- build scripts, tests, and supporting documentation

## Image Import and Format Support

Current support includes:

- PNG, JPEG, TIFF, and HEIF/HEIC import and inspection
- Apple-supported RAW and ProRAW workflows on macOS
- FITS inspection and conversion paths
- preview generation and export through shared codec paths

Current boundaries:

- broader non-Apple RAW support is still a future expansion area
- SER and planetary-video-oriented workflows are not part of the current public
  snapshot

## CLI Capabilities

The CLI already covers a substantial set of processing operations, including:

- metadata inspection and format conversion
- preview generation
- frame registration
- stacking with multiple methods
- calibration with light, dark, bias, and flat inputs
- histogram, stretch, curves, color, contrast, denoise, sharpen, and
  deconvolution operations
- FITS-oriented commands
- RAW-oriented commands on Apple-supported paths
- line-based workflow execution through `photonstack run`

The CLI is also designed to be automation-friendly through:

- structured JSON output
- stable error reporting
- progress events for long-running tasks

## Core Processing Features

The shared engine already includes practical implementations for:

- image inspection and metadata handling
- frame registration and alignment
- average, weighted, median, sigma-clipped, winsorized, and percentile-based
  stacking
- background extraction and normalization
- histogram- and curve-based tone operations
- denoise, sharpen, and deconvolution baselines
- color neutralization and saturation adjustment
- mosaic-oriented workflows
- drizzle-oriented workflows
- star detection, star mask generation, and star reduction
- artifact-oriented workflows for trails and related cleanup

## macOS App Capabilities

The macOS app currently provides:

- project-based workflows
- import, preview, adjust, and export flows
- edit-history persistence and replay
- undo and redo support
- batch queue handling
- recent projects and templates
- preview caching and cache invalidation
- native panel integration and AppKit-backed image preview behavior
- bilingual English and Simplified Chinese UI support

The app also exposes a growing set of higher-level processing tools around
stacking, calibration, background work, star tools, and advanced adjustments.

## Engineering and Contributor Support

The repository already includes:

- CMake-based builds for the shared engine and CLI
- Swift package organization for app code
- local helper scripts for build, test, formatting, and packaging
- C++ engine, CLI, and Swift package tests

## Current Constraints

The public repository should still be understood with these constraints in
mind:

- it is macOS-first
- Apple-framework behavior can affect parts of the imaging workflow
- some advanced workflows are implemented as conservative baselines rather than
  heavily optimized final versions
- large full-resolution projects can still be memory- and storage-intensive

## Areas Still Evolving

The following areas are still active evolution zones rather than “finished”
product claims:

- broader RAW coverage beyond Apple-supported paths
- more advanced large-project scaling and resource management
- future format and workflow expansions such as richer FITS or other
  astronomy-specific paths
