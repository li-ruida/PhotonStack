# PhotonStack

PhotonStack is a macOS-first astrophotography workspace for taking a capture
sequence from source frames to a reviewable, reproducible final image. It pairs
a native desktop workflow with a shared C++20 processing engine and a CLI for
automation.

![PhotonStack stacking result](docs/images/m31-workflow/03-stacked-result.jpg)

## What it does

- Imports RAW, ProRAW, FITS, TIFF, JPEG, PNG, and HEIF without modifying the
  source files.
- Measures star shape, noise, registration quality, and local anomalies before
  stacking.
- Explains every automatic frame exclusion with the filename, metric, group
  median, and threshold; users can keep, exclude, or restore automatic choices.
- Supports calibration, registration, normalization, Winsorized and other
  stacking modes, drizzle, star detection, masks, and star reduction.
- Builds sky mosaics from overlapping pointings. Import one folder per panel or
  start with stacked masters, inspect the alignment stars, and blend the panels.
- Provides deep-sky development, histogram and curves, background and color
  correction, contrast, denoise, sharpen, deconvolution, and artifact controls.
- Keeps processing history and exports the final image separately from the
  editable project.
- Exposes the core processing pipeline through the macOS app and CLI for
  repeatable workflows.

## Workflow examples

| Workflow | Capture | Output |
| --- | --- | --- |
| [Stack & finish](#real-m31-workflow-200-source-frames) | 200 exposures of M31 | 182 selected frames, 1928 × 3638 TIFF |
| [Sky mosaic](#two-panel-sky-mosaic) | Two overlapping pointings, 90 + 112 exposures | 3913 × 3905 TIFF |

[Explore both workflows on the project site](https://astro.ruida.li/#process).

## Real M31 workflow: 200 source frames

The walkthrough below was completed in the release app with 200 evenly sampled
M31 FITS light frames. No calibration frames were added for this demonstration.
PhotonStack analyzed all 200 files, automatically excluded 18 frames with
elongated stars, stacked the remaining 182, applied a conservative
luminance-preserving color-neutralization pass, and exported a 1928 × 3638 TIFF.

### 1. Import and inspect

The workspace makes the material count and next action visible immediately.
The preview is a non-destructive stretch of one source frame, not a processed
replacement for the original.

![Importing 200 M31 FITS frames](docs/images/m31-workflow/01-import-200.jpg)

### 2. Analyze and review exclusions

Screening is designed to be auditable. In this run, the excluded-frame list
shows the exact cause (`星点拉长`, elongated stars), each frame's eccentricity,
the group median, and the recipe threshold. Exclusion affects only the current
stack and never deletes source files.

![Explainable frame screening](docs/images/m31-workflow/02-analysis-review.jpg)

### 3. Register and stack

The selected 182 frames were registered and combined with Winsorized rejection.
After completion, PhotonStack retains the 200-frame source list and the 18-frame
exclusion record so the result can be reviewed or reprocessed.

![M31 after registration and stacking](docs/images/m31-workflow/03-stacked-result.jpg)

### 4. Develop and correct color

Finishing tools are integrated into the same workspace. This example uses a
light full-image neutralization (`0.35` strength, `0.02` green allowance) while
preserving luminance. Curves, galaxy tonal separation, star control, denoise,
sharpen, and advanced processing remain available for further work.

![Luminance-preserving color adjustment](docs/images/m31-workflow/04-color-adjustment.jpg)

### 5. Export the final image

Export is an explicit final step and reports the completed filename in the
workflow. PhotonStack also writes processing metadata beside the image for
traceability.

![Successful TIFF export](docs/images/m31-workflow/05-export-success.jpg)

### Final image

![Final M31 image from the 200-frame workflow](docs/images/m31-workflow/06-final-image.jpg)

> This is a real end-to-end product walkthrough, not a calibrated scientific
> benchmark. Capture conditions, calibration frames, recipe choices, hardware,
> and output resolution all affect quality and processing time.

## Two-panel sky mosaic

T1 and T2 cover adjacent patches of sky. This walkthrough follows the same 202
FITS exposures from folder import through registration, blending and export.
[Open the interactive mosaic walkthrough](https://astro.ruida.li/?workflow=mosaic#process).

### 1. Import the pointings

Choose **Sky mosaic** and select the parent directory or the individual panel
folders. The sidebar groups the 90 exposures in `T1_sub` and the 112 in
`T2_sub` as two panels. If each pointing already has a stacked master, import
the two masters directly.

![T1 and T2 listed as separate panels with their exposure counts](docs/page/assets/mosaic/01-panels.webp)

### 2. Preview the star alignment

PhotonStack stacks the exposures within each panel, then registers the two
masters using stars in their overlap. Toggle the star markers to inspect where
the panels meet before building the full-resolution image.

![Alignment preview of the two overlapping sky panels](docs/page/assets/mosaic/02-align.webp)

### 3. Build the mosaic

The full-resolution build reused both panel masters. The quality report
recorded 223 star matches and zero fallback panels. Exposure matching and
feather blending combined the fields across the overlap.

![Full mosaic and its registration report](docs/page/assets/mosaic/03-stitch.webp)

### 4. Adjust the combined image

This example raises exposure by 0.35 EV across the assembled field to bring
out the faint filaments.

![Exposure adjustment on the assembled mosaic](docs/page/assets/mosaic/04-finish.webp)

### 5. Export the image

The final TIFF measures 3913 × 3905 pixels. The export confirmation shows the
filename, and the project retains the panel groups and edit history.

![Completed TIFF export from the sky mosaic workflow](docs/page/assets/mosaic/05-export.webp)

![Final two-panel sky mosaic](docs/page/assets/mosaic/final.jpg)

## Architecture

- **Core engine:** shared C++20 image-processing and astronomy pipeline.
- **macOS app:** native project, review, processing, history, and export
  experience.
- **CLI:** scriptable inspection, conversion, workflow, FITS, and artifact
  operations.
- **Project model:** source references, recipes, review decisions, generated
  artifacts, and edits remain separate from original media.
- **Tooling:** tests, build scripts, packaging helpers, and documentation live
  in the same repository.

## Documentation

Start with [`docs/README.md`](docs/README.md) for the documentation map, build
instructions, command references, and design notes.

## License

PhotonStack is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE)
for details.
