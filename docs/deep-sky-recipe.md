# Shared deep-sky workflow

The desktop **Deep Sky Assessment & Stack** window and the `deep-sky` command use the same versioned recipe and processing engine. The desktop reads the setting schema from its bundled executable, stores settings and frame overrides with the project, and imports/exports the same recipe format as the CLI.

## Run and review

```sh
photonstack deep-sky schema
photonstack deep-sky --write-default-recipe recipe.txt --input /path/to/lights
photonstack deep-sky --inspect-recipe recipe.txt
photonstack deep-sky --recipe recipe.txt --output-dir /path/to/assessment --analyze-only
photonstack deep-sky --recipe recipe.txt --output-dir /path/to/result
```

Use a new output directory for each run. The recipe is UTF-8 text beginning with `PHOTONSTACK_DEEP_SKY_RECIPE 1`, followed by setting names and double-quoted values. Repeat `input "absolute/path.fit"` for each light in sequence order. Backslashes and quotes are escaped with a backslash. Relative paths resolve against the recipe's directory. Unknown keys, duplicate settings, nonfinite numbers and incompatible settings are errors. Inspecting a recipe validates and canonicalizes it without processing images.

The App's **Analyze** action registers the lights and reports quality without writing a full aligned sequence. Review individual exposures in the assessment tab and choose Auto, Keep or Reject. This changes inclusion in processing; it never deletes originals. A complete run reassesses the original data using the current recipe, so an earlier report cannot silently apply stale transforms or thresholds. The report remains available if the minimum-retention safeguard stops the run.

### Frame review in the desktop App

The window opens on the review workbench. Use **Attention**, **Excluded** or **Manual** to narrow the list, search filenames or translated reasons, and sort by star width, noise or coverage. Select a frame to inspect its original; Command/Shift selects multiple rows. **Keep**, **Exclude** and **Reset to auto** apply to the selected rows. The next-stack count updates immediately; it does not overwrite the saved last-run measurements. **Undo decision** reverses recent decisions in the review view. Command-K keeps selected frames and Command-R excludes them; Option-Up/Down moves to the previous/next frame.

The preview preserves the original pixel dimensions and offers Fit, 1:1 and 2× inspection. **Reference** switches to the reference exposure at the same zoom. These are separately stretched, unregistered previews for inspecting star shape and trails, not a photometric comparison or registered blink test. Viewing them never replaces the editor's current image or adds a processing operation. Preview generation uses a single `review-preview` invocation, with the selected FITS demosaicing algorithm and the existing display transfer. Full-resolution 8-bit TIFF previews avoid a 16-bit intermediate file and PNG compression; the final display precision is unchanged. ImageIO decoding runs away from the main actor. The temporary preview cache keeps at most four images, refreshes its LRU order on access, and distinguishes source revisions and decoder settings. Each native viewer separately retains at most two decoded images within 96 MiB. Both caches are released with the review view.

Changes to the processing recipe mark the prior measurements as stale; changing only manual frame decisions does not. The interface explains the minimum-retention safeguard before allowing a stack with too few frames. Unusable frames cannot be forced into the stack. Advanced processing settings and recipe import/export remain available alongside review.

Reports identify complete star counting with `starCountEstimator: "deduplicated-detection-v2"`. The count includes all separated detections on the bounded analysis image (approximately 1 MP maximum), while the brightest 1,000 detections remain the samples for native shape fitting. It is a relative exposure-quality measurement, not a count of every celestial source in the full-resolution frame. Older reports stopped counting at 1,000; the App displays these capped values as `≥1000` and offers an explanatory tooltip. Reanalyze to update measurements; opening an old report does not rewrite it. Complete counting restores sensitivity of the relative `few-stars` criterion in rich fields, but does not itself sharpen a stack.

## Stages

1. Optional dark, bias and flat masters. Calibration is linear and preserves negative samples. FITS calibration runs on sensor samples before CFA interpolation, using the original Bayer pattern and offsets. Camera RAW calibration uses the existing linear RAW decoder. Explicit bias-included/removed states prevent subtracting bias twice.
2. Scientific decoding, default Malvar FITS CFA reconstruction, and native quality measurements. An automatically chosen reference or explicit zero-based reference index defines the output coordinates. Similarity registration and bicubic interpolation are defaults. A frame is interpolated once; the stack does not register it again.
3. Frame assessment and manual overrides. Invalid decoding/registration cannot be overridden into a valid frame. The default selection safeguard requires at least three frames and half of the input sequence.
4. Additive background normalization and stack rejection. Sigma clipping defaults to three iterations of weighted median/MAD rejection at ±3 sigma. Other stack methods remain selectable; MAD is only valid for Sigma, and average methods have no rejection maps. Weights follow recipe input order.
5. A largest common-coverage rectangle, optional extra inset, polynomial and/or grid background correction, protected linear nonlocal and multiscale denoising, and astronomical development to a full-resolution 16-bit TIFF. There is no forced aspect ratio, fixed channel-curve chain or repeated star shrinking.

### Channel alignment and low-SNR display color

After common-coverage cropping, `develop.align-channels` (on by default) estimates red/blue displacement relative to green from compact stellar Gaussian fits. A correction requires at least 12 consistent matches spread over at least three image quadrants, low median scatter, and a significant shift between 0.05 and 1.5 pixels. Spatially inconsistent offsets, insufficient stars and already aligned channels are left unchanged. This corrects a global translation, not field-dependent aberration or every CFA artifact. Normalized separable Lanczos-3 shifts only the accepted color channels; green and alpha are unchanged. Samples beyond the image boundary use reflection. Original stack files are never overwritten. `report.json` records accepted shifts, scatter and sample counts for each channel.

`develop.shadow-neutralization` (workflow default 0.8, 0 disables) reduces display chroma when the linear signal is weak relative to three robust sky-noise sigmas. It retains the luminance transfer exactly and leaves high-SNR color almost unchanged. This changes the rendering, not the linear master, and may reduce real but low-SNR faint color; it does not recover missing detail. The standalone `develop --shadow-neutralization` option defaults to 0 for explicit command-line reproduction. The App exposes this as **Background color noise** in development controls and preserves it in operation replay; historical records without the field retain their prior rendering.

### Background and correlated color noise

`background.extrapolate-edges` continues the grid surface by at most half a cell beyond its outer sample centers. This avoids leaving the image border uncorrected on a sloping background. Target exclusion ellipses apply to both the polynomial and grid stages, in the original reference-frame coordinates. Grid resolution should remain coarse enough to distinguish sky variation from the subject; exclusion regions are particularly useful when increasing the grid density.

`denoise.multiscale-luminance` and `denoise.multiscale-chroma` control separate linear luminance and color-difference reductions. Defaults are 0.6 and 0.8 across four scales; zero disables that component. The transform uses separable B3 spline smoothing, measures MAD independently in each band to account for correlated noise, retains the coarsest residual, and protects the neighborhoods of significant structures. Strong coefficients and zero-weight protected pixels are retained. Color-only processing preserves linear luminance up to floating-point rounding. The original linear master remains a separate product.

The C++ `MultiscaleDenoiser::apply` also accepts an optional noise realization for
band-noise estimation. It must be signal-free linear RGB with matching geometry,
units, amplitude and spatial correlation at the stage being processed. Its pixels
are used for band statistics only. This avoids estimating noise from scene texture
when an independently calibrated noise sample is available. Geometry, finite
samples and full coverage are validated; scientific correspondence remains the
caller's responsibility. This engine capability is not yet generated or selected
by the App/recipe workflow. In particular, a noise realization before nonlinear
denoising is not a valid substitute for noise after that operation. With no
reference supplied, the existing processing path is unchanged.

The workbench's **Reprocess master / 更新背景与降噪** action reuses the last completed scientific master when inputs, selection, calibration and stack settings match. Only crop, background, denoising and development settings may differ. The App checks source sizes and nanosecond modification times before reuse, keeps the original assessment and rejection-map provenance, and writes a new output directory. An imported recipe with the same input sequence retains the previous report so this route remains available. A new analysis-only report does not itself provide a reusable master.

Changing finishing settings keeps the frame assessment and planned count valid. The workbench and overview identify the displayed image as the previous result until reprocessing succeeds (or the original settings are restored). Changing manual selections also leaves frame measurements valid, but requires a new stack. Source, calibration, registration and assessment-setting changes still invalidate the assessment. Saving diagnostic denoise stages affects file retention only and does not mark the rendered image out of date. Older recipes that omit the scene-based noise estimator use the same default as an explicit `denoise.noise-model "scene"`.

`denoise.background-strength` optionally reduces residual background texture after multiscale denoising. Its range is 0–1 and the default is 0 (disabled). A normalized B3 background estimate excludes protected sources; a common luminance/chroma significance weight retains strong residuals. A separate small-footprint detector protects weak compact sources using noise measured at that spatial scale. This avoids relying only on the pixel-scale Haar noise estimate for correlated data. No clipping or resampling is applied, and alpha is unchanged.

The background-extraction exclusion ellipses also protect their original pixels in this stage, with a feathered transition outside the ellipse. Coordinates remain in the original reference frame and account for the coverage crop. The output is `stack-background-denoised.fits`; the original linear master and preceding denoising outputs remain separate. Inspect faint diffuse structures before increasing strength: residual smoothing cannot distinguish every low-contrast astronomical feature from noise. It is not a replacement for calibration frames or additional exposure.

For controlled comparisons, the CLI exposes the same finishing implementation:

```sh
photonstack deep-sky --recipe recipe.txt --from-master stack-linear.fits --output-dir new-finish
```

This command does not re-assess or re-stack the exposures: its report marks `masterReused`, and contains no newly measured frame records. The caller must supply the matching scientific RGB master; CFA raw frames and display images are rejected. The App retains an additional `app-report.json` with the original frame assessment attached to the new finish. Missing cached outputs can be regenerated by replaying the complete saved recipe.

Per-band noise measurement and processing before display stretching are also discussed in the [Siril wavelet commands](https://siril.readthedocs.io/en/latest/Commands.html) and [denoising documentation](https://siril.readthedocs.io/en/latest/processing/denoising.html). This implementation is independent of Siril's denoiser.

The App exposes all fields returned by the recipe schema. Color/development controls, interpolation, normalization, rejection thresholds, frame weights, calibration paths and target-exclusion ellipses therefore use the same values on both entry points. Specialized standalone CLI operations and the legacy editor are separate from this recipe; their settings are not implicitly appended to its result.

## Interpreting assessment

Scientific noise is measured with a local diagonal high-pass MAD estimator in input units. It rejects a planar gradient and is not a scene-wide min/max-normalized brightness spread. Stellar detection for assessment uses median/MAD thresholds so an isolated bright trail does not suppress stars throughout the image. Stellar width and shape are measured on a bounded image and reported in native-pixel units; they are comparative quality metrics, not a calibrated optical PSF measurement. Detection retains at most 1,000 stars, so the reported count is not a full star catalog.

After registration, additive sky levels are removed before comparing each sampled pixel with the median of the other covered frames. Both positive and negative residual fractions are reported. Persistent nebula structure is present in the comparison frames and is not, by itself, a transient. Too few comparison frames are explicitly reported as insufficient evidence.

Broad residuals, excessive noise, enlarged/elongated stars, low relative star count or poor coverage may recommend excluding a frame. Sparse anomalies instead recommend pixel rejection. Narrow lines in sampled positive/negative residuals are reported as **transient line candidates**, with sign and endpoints. This is not reliable identification of a satellite, aircraft or meteor. The bounded grid can miss thin or short trails; full-resolution pixel rejection operates separately and does not depend on these candidates. Review suspected trails in the originals and final rejection maps.

### Comparing finished images in the App

**Compare results / 成片对比** opens a separate viewer for two developed TIFF, PNG,
JPEG or HEIF images. When viewing the latest deep-sky result, the viewer suggests
the preceding available result from the project history. After Diffuse tone,
Color cast, or Display grid reduction, it instead suggests that adjustment's
recorded input, so the before/after pair is ready without finding files manually.
The current output must match an enabled history operation and both display files
must still exist. Files can also be chosen explicitly, without importing them into
or changing the project.

Both full-resolution images are decoded before switching is enabled. Fit, 1:1
(one source pixel per physical screen pixel) and 2× views share the same viewport;
the Space key switches images without resetting zoom or pan. Different dimensions
are rejected instead of silently resized. Equal dimensions do not establish
registration: use results with the same orientation and crop. Compare diffuse
structure and star profiles as well as grain; a cleaner background alone does
not demonstrate improved detail.

## Outputs and storage

Full runs retain `stack-linear.fits`, `stack-background.fits`, `developed.tiff`, optional `rejection-low.fits`/`rejection-high.fits`, calibration masters when supplied, `recipe.txt` and `report.json`. The rejection images give per-channel rejected fractions of covered input samples. Per-frame report counters distinguish compared samples from low/high rejected samples; Winsorized counters describe samples bounded rather than removed. The linear stack retains coverage in alpha. Background fitting and denoising precede display development.

Aligned images and the stack's temporary caches belong to `.aligned-work` inside the run directory and are removed after success or handled failures. App cancellation also removes this owned directory when a terminated process cannot unwind its cleanup. Reports and final masters are retained. A hard-killed CLI process may leave its run directory's `.aligned-work`; it can be removed once that process has stopped. Original light and calibration inputs are never cleanup targets.

During normal processing, rejected aligned frames are removed once selection is final. Each selected aligned FITS is released after its complete internal stack cache has been written and closed (or its pixels have been accumulated for averaging). This avoids retaining two full copies of the aligned sequence. The robust stack checks available space for each next cache frame, including a reserve; it does not assume that future releases have already happened. Default direct `Stacker` calls retain their input files and still preflight the whole cache. The optional `inputConsumed` callback reports the original input index, rejects duplicate canonical input paths, and transfers no ownership of user files to the engine.

When stellar measurement is explicitly disabled, the review does not label that choice as a measurement failure. Requested measurements that are unavailable continue to produce a warning.

App results go to the saved project's `processing` directory, or its managed preview directory before the project is saved. The project stores absolute output references. Moving a project folder alone does not currently relocate those references; preserve its managed files or use the existing relink workflow. The recipe retains original input paths for replay.

The preview decoder and tone mapping use bounded CPU parallelism for large images (up to eight workers). CFA reconstruction reads an immutable sensor mosaic and writes separate output rows, retaining the same edge and invalid-sample fallback. Opaque-image stretch statistics reuse a single double-precision luminance buffer; fractional coverage still follows the weighted path. The `review-preview` completion event reports decode, display-transfer, stretch, and write timings in milliseconds. These timings describe preview generation, not an entire stack or total GUI latency.

### Native stellar shape measurements

Quality analysis uses a bounded detection image to locate candidate stars, then
fits up to 128 accepted elliptical Gaussian profiles plus planar sky on the
original pixels. Widths are `2.354820045 * sqrt((covXX + covYY) / 2)` in native
pixels; eccentricity comes from the fitted covariance eigenvalues. Detection
resize blur is not part of the reported width. Clipped, masked and poorly fitted
sources are rejected, and duplicate native centers are counted once.

Frame reports record `fwhmEstimator: native-gaussian-v1` and `starShapeCount`.
At least five fits are required for shape medians. Missing shapes display as
unavailable and raise a review warning; they cannot win reference selection by
appearing to have zero width. Detection counts and shape-fit counts differ.
Existing reports remain readable and identify their earlier width estimates in
the App's help text. New analyses replace the old measurement method. Reference
choice and width/elongation screening may consequently change; review those
choices before comparing runs. These fitted widths support relative quality
checks and are not an absolute optical resolution calibration.

## Optional sensor-fixed texture correction

`calibration.sensor-pattern "on"` estimates a fine-scale additive CFA residual from
36–60 selected exposures and subtracts it before demosaicing. It defaults to off.
It requires matching camera, exposure, gain, geometry and Bayer metadata, full
sensor coverage, and measurable field movement. Dark/bias/flat combinations are
currently rejected. It does not estimate a full dark frame or repair large-scale
background structure.

Three interleaved groups estimate three models. Each training exposure receives a
model trained only on the other two groups; external exposures use a matching
model from the same run. Selection and transforms are measured before correction,
then replayed with corrected CFA data. Only the corrected aligned sequence is
written. Source SHA-256 values and applied model IDs are recorded in the report;
`sensor-pattern.txt` records training indices, positions, model digests and fit
statistics. Models are never loaded from unrelated cached runs.

The `cfa-highpass-crossfold-v2-soft-confidence` estimator gradually reduces
sample weights around bright structures instead of abruptly excluding samples.
It uses a robust weighted location and fades correction where support is weak;
the documented minimum of 36 exposures remains supported. This addresses
collateral changes caused by exclusion-mask threshold crossings, not optical
blur. Its temporary cache stores a value and confidence per sensor pixel; the
cache-space check includes both, and the run removes its owned cache on return.

Analysis-only measures acquisition and registration without estimating or applying
the correction. The full run enforces sample count and movement requirements.
`--from-master` rejects this option because it requires original CFA samples. The
App can reprocess an already corrected master: it verifies source hashes and
retains its original correction provenance while disabling a second application
in the finish-only invocation.

## Extended targets and background exclusions

`background.protect-targets` selects the lower quartile instead of the median
inside each grid cell. It reduces contamination by bright pixels; it does not
automatically trace a galaxy or nebula. For an extended target, supply
`background.exclusions` covering its faint outskirts as well as the bright core.
Ellipses use the original master coordinates and semiaxes; the workflow adjusts
their positions when cropping. Both polynomial and grid models omit these samples.

In the App, open **Processing settings → Background → Edit regions on image**
after a successful stack. The editor overlays the saved ellipses on that result,
accounts for its crop, and writes coordinates in the original reference canvas.
Drag to draw a new ellipse, select one to move it, or edit its center, semiaxes,
and angle numerically. Cancel discards the draft; **Save regions** changes the
recipe only. Use **Reprocess master** to generate a new result, then compare it
at native scale. Missing crop metadata or a result-size mismatch disables the
editor instead of guessing coordinates. Leave enough unprotected sky to measure
the background. The overlay describes the exclusion area, not a live preview of
the corrected background model.

A grid cell with fewer than 25% usable pixels after explicit exclusions is
treated as unmeasured and filled from valid sky cells. This prevents a few bright
pixels at an exclusion boundary from defining the background under a target.
Exclusion holes are filled even when the lower-quartile option is disabled.
If no usable cells remain, the operation fails instead of inventing a model.
Inspect the whole field and target at native scale after changing the model or
exclusions; preserving broad emission is distinct from recovering fine detail.

## Experimental independent luminance noise estimate

`denoise.noise-model "independent-luminance"` estimates luminance noise from
alternating independent exposure groups during Sigma stacking. It can reduce
suppression of diffuse structure that would otherwise enter the scene-derived
noise estimate. It defaults to `scene` and does not change chroma thresholds.
Set `denoise.h "0"`; the reference cannot follow unmodelled nonlinear denoising.
First use requires a new stack with at least six selected exposures. A retained
three-file noise bundle then permits finish-only reuse with the same crop and
measured channel transform. Invalid or mismatched references fail explicitly.
See [the diagnostic contract and limitations](stack-noise-diagnostic.md).

This is an experimental opt-in capability. Field motion and inter-group
correlation do not prove that all extended target structure is excluded. Review
native-scale faint structure, full-field background and color against a matching
uncorrected run before accepting a result. Lower noise alone is not a claim of
recovered resolution.

### Comparing diagnostic sub-stacks

Keep calibration and background normalization consistent with the run being
evaluated. A crop made from uncorrected exposures is not a control for an App run
that applied a sensor-pattern model. Background normalization also depends on
the sampled field: measure it on the full registered frame before extracting
diagnostic regions, then disable a second normalization on those regions.

Disjoint exposure membership does not remove dependencies introduced by a shared
calibration model. To measure this effect, distinguish sub-stacks using the exact
run-owned model from sub-stacks whose models were trained separately within each
exposure group. Record training membership and model digests, and retain the
builder's excluded-fold rule for every training exposure. Reused registration
and source-selection measurements still limit statistical independence.

Bind a diagnostic set to the estimator implementation as well as the recipe:
record the source/build digest, training inputs and resulting model digests.
An unchanged recipe version does not make sub-stacks from an older estimator
equivalent to the installed engine. If exact model reproduction fails, establish
the version boundary before reusing the set; regenerate the baseline and
candidates with the same implementation. Keep older results labeled with their
original processing version instead of silently treating them as current.

Also distinguish the installed engine's replay from the result currently stored
in the App project. Verify the displayed output path, crop and digest when
labeling a comparison. Installing an engine update does not reprocess an existing
project image automatically.

Inspect native-scale diffuse structure as well as point sources. Repeated
fine-scale texture can contain residual fixed-pattern noise; a lower prediction
error can also result from excessive smoothing. Neither is sufficient evidence
of recovered detail or improved finished-image quality.
