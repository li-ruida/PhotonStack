# Image Pipeline

PhotonStack is built around a processing pipeline that tries to preserve weak
astrophotography signal while keeping workflows reproducible across the app and
CLI.

## Core Principles

- move imported images into a well-defined working representation early
- keep high-bit-depth, linear-friendly processing paths where practical
- separate preview behavior from final full-resolution output
- prefer non-destructive processing steps over in-place edits

## High-Level Flow

The end-to-end workflow is conceptually:

```text
import
  -> decode
  -> metadata extraction
  -> optional calibration
  -> quality analysis
  -> registration
  -> stacking or reconstruction
  -> background and tone work
  -> color and detail work
  -> export
```

Not every operation uses every stage, but this is the overall shape the
repository is designed around.

## Import and Decode

The repository currently supports workflows built around:

- common image formats such as PNG, JPEG, TIFF, and HEIF/HEIC
- Apple-supported RAW and ProRAW paths on macOS
- FITS-oriented inspection and conversion flows

Import behavior is designed so that processing, preview, and export do not
quietly drift across different pixel conventions or orientation rules.

## Working Representation

PhotonStack’s engine is designed around internal image representations that are
appropriate for processing, analysis, and reproducibility rather than only
display.

That matters because many astrophotography operations become fragile if the data
is reduced too early to low-bit-depth, display-oriented formats.

## Calibration and Analysis

The processing pipeline is intended to support workflows such as:

- light / dark / bias / flat calibration
- quality scoring
- star detection and frame analysis
- registration and alignment

These stages are important not just for image quality, but also for reliable
automation and repeatable batch behavior.

## Stacking and Reconstruction

The current repository already contains support for multiple stack-oriented or
reconstruction-oriented paths, including:

- average and weighted stacking
- robust multi-frame methods such as median and clipping-oriented approaches
- registration-aware processing
- drizzle- and mosaic-oriented workflows

These are all part of the same broader engine goal: use shared processing logic
for both interactive and scripted workflows.

## Post-Stack Processing

After alignment or stacking, the pipeline can include:

- background extraction
- normalization
- stretch and curve adjustments
- color adjustment
- contrast enhancement
- denoise and sharpen operations
- deconvolution baselines
- star-mask and star-reduction workflows
- selected artifact-handling workflows

## Preview vs Final Output

A key design constraint is that preview behavior and final output behavior are
not identical concerns:

- previews should stay responsive
- final output should remain faithful to the real processing graph
- cache layers should not silently replace full-resolution recomputation when
  correctness matters

That separation is especially important for large astrophotography projects.

## Performance Considerations

The repository is designed with long-running and large-image workflows in mind.
Important constraints include:

- memory pressure from full-resolution multi-frame processing
- storage pressure from caches and intermediate results
- the need for cancellation, progress reporting, and resumable-friendly design

Optimization work should continue to follow a “correct first, then measure, then
accelerate” approach.

## Scope Notes

- The current public repository is macOS-first.
- Some format behavior depends on Apple platform imaging support.
- Advanced performance optimization and some broader-format workflows are still
  active evolution areas rather than finalized product claims.
