# App / CLI processing parity

## Acceptance contract

The App and CLI must execute the same versioned deep-sky recipe, including scientific decoding, registration, frame assessment/selection, pixel rejection, background correction, denoising, development and cropping. A GUI result is not equivalent merely because it uses the same executable. Recipe persistence, selection decisions and full-resolution exports must agree. Original inputs are never deleted by screening.

Robust combination partitions each bounded cache tile into independent pixel
ranges, using at most eight workers. Per-pixel sample order and floating-point
reductions stay unchanged; per-worker integer rejection counts are merged after
joining. Progress callbacks remain on the caller thread, and cancellation still
cleans owned caches. The engine's `StackOptions.combineWorkers = 1` provides a
serial verification path. This scheduling control is not a scientific recipe
parameter and requires no additional aligned image files. Regression tests compare
master, rejection and optional noise-diagnostic pixels bit for bit, including
weights and incomplete coverage. A combine-stage benchmark is not a claim about
whole-workflow speed or image sharpness.

Cache readers retain sequential file positions and share an explicit read-buffer
budget of at most 64 MiB, separate from the existing tile budget. Buffers are set
before opening and outlive their streams. This avoids discarding prefetched bytes
between adjacent row ranges; short reads still fail the operation. A multi-tile
test checks every output pixel against a known row-dependent signal, including a
short final tile, independently of serial/parallel comparisons.

## App navigation

The **Process** inspector contains the deep-sky workbench and image adjustments. **Assets** contains frame roles and metadata, **Tools** contains independent registration, mosaics, layers and batch operations, and **Project** contains history and project settings. The former automatic-workflow controls and automatic-stretch action are removed from the App; deep-sky processing uses the shared recipe and development controls. Historical project fields and replay support remain readable.

Open the workbench from **Process → Deep Sky Assessment & Stack** (`Shift-Command-D`), the primary canvas action, or the workflow summary. These routes share one presentation. Empty projects can import a light folder from the workbench. **View stack result** returns to the latest enabled, available deep-sky output without processing it again or adding an edit operation; running a new analysis does not discard that result.

During the first run, the empty review screen becomes a processing view showing
the active stage and elapsed time. Elapsed time uses the job's start date, so
closing and reopening the workbench does not reset it. Closing the panel leaves
the job running; **Cancel** remains the explicit stop action. Existing review
results stay visible on later runs, with compact progress below. At engine 100%,
the view says the result is being saved and loaded until the App finishes its work.

**Compare results** is also available inside the workbench. Newly processed App
results retain the crop rectangle and SHA-256 identities of the linear master and
developed file in `app-report.json`. When the current and previous results come
from the same master, the comparison extracts their common integer pixel region
and opens at 1:1. Different crop margins do not trigger resizing or another
registration. Switching preserves pan and zoom and does not modify either file.
Changed output bytes, inconsistent crop dimensions, disjoint crops or different
masters prevent automatic coordinate matching. Legacy results remain readable;
without the required provenance, users can explicitly select images whose framing
they have verified. Manual comparison checks dimensions but cannot prove matching
framing. Reprocessing also checks a recorded master digest before and after use.

Required evidence before completion:

1. One shared processing implementation and recipe, with strict validation and App controls for every supported recipe setting; recipe import/export and persistence survive reopen.
2. Per-frame scientific noise, stellar width/shape, registration/coverage, signed temporal residuals and transient-trail findings. Frame inclusion can be reviewed and overridden, with invalid inputs kept unusable. Sparse transients are handled by pixel rejection, not automatic loss of the whole exposure.
3. Shared signed pixel-rejection and coverage diagnostics, minimum-frame safeguards, invalid/partial overlap handling, no extra registration interpolation.
4. Linear FITS preserved through scientific stages; matching background, protected denoising, development and explicit crop settings. No hidden fixed color curves, repeated star shrinking or forced landscape crop.
5. Synthetic tests for bad registration, blur, positive/negative outliers, changing sky levels, masked edges and persistent nebulosity; App/CLI recipe and output parity tests.
6. Same-input/same-recipe real NGC7293 runs through the CLI and actual App controls; image/metric comparisons and native-scale visual review. Install in place, preserve source hashes, clean only disposable run/build intermediates.

## Verified starting differences (2026-09-19)

The prior CLI finish used Malvar CFA decoding, bicubic interpolation, additive background normalization, polynomial target-excluded background fitting, a second grid correction, protected linear nonlocal denoising and controlled asinh development. The App workflow did not forward the first three options. It pre-aligned with default decoding/interpolation, then ran another stack cache, and applied a separate fixed chain of display PNG operations, channel curves, two star reductions and a 16:9 crop. The GUI recipe dropdowns also reset after reopen. The App export was 16-bit, which did not undo those preceding choices.

The existing `FrameQualityAnalyzer` reports scene-wide MAD after per-frame min/max normalization. That noise value conflates sky structure with noise and changes when a single extreme pixel changes the maximum. It is retained for legacy callers; new scientific sequence assessment uses a local high-pass estimator in input units and aligned temporal comparisons.

## Design rationale

Separate frame selection from pixel rejection. Diagnose blur, elongation, poor coverage/registration and broad anomalous residuals at the frame level. A satellite crossing only a small part of a frame should not discard the uncontaminated exposure. Assess positive and negative residuals against other aligned frames, using coverage and noise floors, after additive sky normalization. Report uncertainty when too few comparison frames exist. Do not infer satellite identity from one positive residual alone.

Siril's documentation independently describes input normalization before rejection, separate high/low rejection maps, and frame selection by FWHM, roundness, background and star count: [Stacking](https://siril.readthedocs.io/en/stable/preprocessing/stacking.html). Its algorithms are background context, not a claim that this implementation reproduces Siril.

## Verification, 2026-09-19

The shared recipe was run independently on all 359 NGC7293 exposures through the CLI and the installed App's **Run stack** button. All five compared files were byte-identical: the linear master, background-corrected master, developed 16-bit TIFF, low rejection map and high rejection map. All per-frame measurements, decisions, rejection counters, reference index and crop also matched. The only recipe text difference was an empty override list versus 359 explicit `auto` entries, which have the same meaning.

All 359 frames passed the conservative frame thresholds; 40 had localized-transient warnings. A bright-trail frame that the initial mean/standard-deviation star detector misclassified as having too few stars passed after robust detection was introduced. Its local outliers remained eligible for full-resolution rejection. No transient line candidates were identified in this real sequence by the bounded assessment grid; thin trails remain a documented limitation of that diagnostic.

The output is 1922 × 3622 with complete input coverage throughout the crop. The full linear master remains 2160 × 3840. Rendered TIFF crops and the App's 1:1 display were inspected. The nebular ring and central stars are visible without the old forced landscape crop or purple display chain. Background color mottling, edge gradients, some stellar color fringes and limited faint structure remain. Output parity does not establish a resolution gain or scientifically calibrated color.

The latest application was installed in place at `/Applications/PhotonStack.app` (build `20260919062144`). The saved project, recipe and assessment reopened successfully. A new analysis was started and cancelled through the GUI; cancellation remained available, removed its owned cache, and preserved the prior completed result. The real parity runs used the immediately preceding build; the final update adds UI cancellation/progress/relink fixes and a separately tested masked-CFA calibration fix outside this uncalibrated dataset's path.

Validation includes 29 C++ test executables, a serial 424-test Swift suite, and a subsequent additional cancellation test plus seven focused workflow/settings tests after the final UI changes. Evidence is retained locally under `artifacts/ngc7293/parity/reports/`, including hashes, recipes, source integrity checks, screenshots, test logs and cleanup records. Duplicate CLI image outputs are removed after comparison; reproducing the comparison requires rerunning the CLI recipe into a new directory. The saved App project and its full-resolution outputs remain available.
