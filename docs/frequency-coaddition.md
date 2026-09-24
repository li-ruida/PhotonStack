# Experimental frequency coaddition core

`FrequencyCoadd` accumulates registered, fully covered, background-subtracted
scalar image planes using a known per-exposure PSF, positive throughput and
stationary noise power. It preserves double-precision scientific values, including
negative samples. It is not currently selected by the ordinary `stack` command.

For image transform F, PSF transform P, throughput f and noise power V, each frame
adds `f * conjugate(P) * F / V` to the numerator and `f² * |P|² / V` to the
information. Rendering with unit-DC target T applies `numerator * T / information`
and a normalized inverse real FFT. Missing information with a nonzero requested
target fails; the core does not silently invent those frequencies. Choosing a
sharper T can amplify noise and does not establish recovered optical resolution.

The caller supplies a common padded grid. Registration, sky/throughput estimates,
PSF estimation, target selection, clipping detection, rejection and coverage
handling belong to separate processing stages. The current experiment uses
whole-sample reflected padding; the accumulator itself does not pad or crop.
The half spectrum is row-major `height × (width/2 + 1)`.

### Combining previously clipped groups

Separately Sigma-clipped groups are not interchangeable with linear exposure
averages. Each group estimates different rejection limits and retains a different
set of samples at each pixel. Weighting their images by the original frame counts
does not pool the surviving samples correctly. Weighting by per-pixel accepted
counts pools those survivors, but does not restore samples discarded under the
different group-specific limits. Neither operation reproduces a single joint
Sigma stack in general.

The M31 grouped-input trial reproduced native Sigma pixels exactly from the same
calibration and registration, while exposing this difference before frequency
weighting. Removing the stellar gain fit did not resolve its core prediction
error. Therefore a fitted Gaussian PSF, valid stellar photometry and numerical FFT
parity are insufficient grounds to adopt a clipped-group frequency result.

For grouped-input experiments, retain the joint Sigma baseline, an unclipped
linear regrouping control, and rejection/count diagnostics. Test preprocessing
before evaluating frequency weighting. A common rejection decision or explicit
per-exposure preparation may remove one inconsistency, but still needs noise,
effective-response and native diffuse-detail validation; spatially varying
rejection is not made stationary by renaming it a prepared input.

Local evidence and reproduction scripts are in
`artifacts/grouped-attribution/20260923/REPORT.md`; experimental data are not
required runtime dependencies. This limitation does not change the ordinary
App stack method or claim that joint Sigma rejection causes the displayed blur.

`StackOptions::acceptedGroups` provides an optional C++ preparation primitive for
checking a common rejection decision. Supply original-input-order labels 0/1,
including both labels, for distinct scientific linear RGB inputs with SigmaClip.
The ordinary joint master and its rejection/noise reports are unchanged. The
result's `acceptedGroupImages` and `acceptedGroupWeights` contain two RGBA means
and per-channel sums of accepted `normalized frame weight × coverage`. Their
alpha is 1 only when every RGB channel has positive support; a missing channel
has mean and weight zero. Other valid channels are retained even when alpha is 0.
Pool channels using the weight maps, rather than dropping a whole pixel based on
this diagnostic alpha or using the original frame counts.

The two groups use the joint estimator's bounds, including its global all-rejected
fallback; an empty individual group never invents its own fallback. Group labels
continue to refer to original input order when registration reorders its cache.
The outputs are opt-in and are not part of the saved deep-sky recipe, CLI or App
default workflow. They share a data-dependent selection and must not be treated
as independent noise references. Recombination can reproduce the joint estimate
within output rounding; it does not by itself improve image quality or establish
a fixed spatial response suitable for frequency coaddition.

`psfTransfer` normalizes an odd empirical kernel, embeds its center at zero lag,
applies the requested fractional-centroid phase, and projects Nyquist bins back
onto a real spatial image. Small negative empirical PSF lobes are retained.
`noisePower` expands each short covariance displacement and its negation and
reports how many spectral bins were floored at 0.05. The floor is an explicit
experimental model constraint, not rejection of bad science pixels.

`add` and `merge` validate inputs and stage updates before committing their
state. Invalid shapes, nonfinite values, invalid real-spectrum boundary symmetry,
nonpositive noise/throughput, unsupported targets and arithmetic overflow fail.
An instance requires external synchronization if accessed concurrently. FFT
parallelism is bounded by its constructor thread count. Grid dimensions are
limited to 16,384 per axis and 16,777,216 total pixels to bound allocations.

PocketFFT is vendored at a fixed upstream commit, with source hashes and license
notices recorded in `third_party/pocketfft-source.json` and the dependency
register. No Python runtime is needed by this C++ core. CMake and SwiftPM include
the same implementation. The `frequency-stack` CLI reads explicit prepared-input
plans and exports RGB scientific FITS. Model estimation and preparation still use
external scripts; this is not yet a complete App workflow.

## Prepared-input CLI

```sh
photonstack frequency-stack --plan prepared.psfreq --output linear.fits --threads 2
```

This experimental command processes three channels sequentially to limit memory.
It reads registered RGBA float32 caches, applies each channel's sky subtraction in
double precision, pads the crop by whole-sample reflection, accumulates known-PSF
frequency estimates, applies the supplied target, and crops back to the requested
field. It writes float32 RGB FITS with a fourth, unit-coverage plane, without
display normalization or clipping.
Negative values and values above the sensor range are retained; overflow fails.
No WCS or exposure metadata is inferred for the cropped output.

The versioned UTF-8 plan has the following fixed token order. Newlines separate
fields for readability; other whitespace is equivalent. Comments and unknown
fields are not accepted. Every path must be double-quoted. Backslash escapes a
literal backslash or quote (`std::quoted` syntax, not JSON escape syntax).
Relative paths resolve against the plan directory, independent of the current
working directory. Numeric fields are finite decimal values parsed as double
precision unless an integer is required.

```text
PHOTONSTACK_FREQUENCY_PLAN 1
source-rgba-f32le 2160 3840
crop 132 132 1896 3576
padding 204 260
target-real-f64le "models/target.f64"
frames 1
frame "prepared/frame-0.f32" 1.02
channel 0 1700 16000 0.05 -0.02 "models/psf-0-r.f64" 31 31 2
lag 1 0 0.3
lag 0 1 0.25
channel 1 2100 18000 0.01 0.03 "models/psf-0-g.f64" 31 31 0
channel 2 1400 14000 -0.03 0.02 "models/psf-0-b.f64" 31 31 0
end
```

- `source-rgba-f32le`: source width and height. Each frame file is a headerless,
  row-major, interleaved R/G/B/coverage array of IEEE little-endian float32 values.
  The science channels must be in a common linear coordinate and photometric
  convention. Each cropped pixel must have finite coverage in `[0.999, 1]`;
  incomplete coverage is rejected rather than filled as measured signal.
- `crop`: zero-based x, y, width, height. `padding`: x and y pixels on each side.
  Reflected pixels are boundary treatment, not additional exposures. Cropping
  away padding does not guarantee the absence of boundary-model artifacts.
- `target-real-f64le`: headerless, row-major real half-spectrum of shape
  `(cropHeight + 2*padY, (cropWidth + 2*padX)/2 + 1)`, with integer division.
  Values are IEEE little-endian float64, finite, unit DC and valid real-image
  boundary symmetry. This version supports a real target response only.
- `frames`: number of frame records. `frame`: prepared path followed by positive
  relative flux throughput. Repeated canonical frame paths are rejected.
- Each frame has exactly three ordered `channel` records (0=R, 1=G, 2=B):
  channel number, sky level, noise variance (not standard deviation),
  PSF centroid x/y in pixels, kernel path, kernel width/height, and lag count.
  The sky and variance refer to the prepared frame's units, before division by
  throughput. The PSF centroid must already be relative to the chosen output
  origin; the command does not estimate or subtract an origin offset.
- PSF files contain row-major little-endian float64 samples, with the center at
  `floor(size/2)`. Dimensions must be positive odd integers no larger than 129
  or the padded grid. Kernels are normalized by their positive sum; signed lobes
  are preserved. Centroids are limited to one quarter of the smaller dimension.
- Each `lag` supplies integer dx, dy and a correlation in `[-1, 1]`. It also
  represents the negative displacement. Zero, duplicate and opposite duplicate
  lags are rejected. Displacements are limited to ±64 and each channel to 256
  entries. The noise-power floor of 0.05 is reported per output channel as a
  count of spectral bins across exposures, not a count of rejected pixels.

Plans are limited to 16 MiB and 10,000 exposures. Source and padded grids are
limited to 16,384 pixels per axis and 16,777,216 pixels total. Inputs have exact
expected byte counts; file size and modification time are checked during the run
and before output. These checks detect ordinary changes, not cryptographic
content identity or adversarial concurrent replacement.

The output must be a new FITS path. The command stages a complete FITS beside
the destination, then atomically creates it without replacing existing files,
hard links or symlinks. A failed run does not publish a partial FITS. The output
filesystem must support hard links. JSON progress records use the standard
`task`, `command`, `stage` and `progress` fields.

This command does not estimate registration, PSFs, target sharpness, covariance,
sky or throughput. It also does not reject temporal anomalies or protect saturated
sources. Those inputs and later stages need independent quality checks; successful
execution alone does not establish an improvement in image quality.

The Swift processing service exposes
`frequencyStack(plan:output:threads:)` for the same prepared-input operation.
It rejects existing destinations before launch, validates the staged output,
and uses exclusive atomic publication so a destination created during processing
is also preserved. Other processing commands retain their existing replacement
behavior. Automatic model preparation and a complete App UI flow are not supplied
by this service method.
