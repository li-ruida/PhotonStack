# Sigma-stack noise diagnostic

`Stacker::stack` can optionally return `StackResult::noiseDiagnostic` alongside
the unchanged scientific master. Collection is opt-in. The deep-sky workflow can
use it for experimental luminance-only noise estimation as described below.

## Contract

- Set `collectNoiseDiagnostic = true`, use `SigmaClip`, and supply at least six
  distinct input paths. Duplicate canonical paths are rejected; the caller must
  also ensure different files represent distinct exposures, not copies.
- Supply one finite positive `frameNoiseVariances` entry per original input,
  representing relative noise variance in the registered linear image units.
  Explicit all-ones asserts equal variance. Exposure weights are separate and
  cannot by themselves establish these variances. A common variance scale cancels.
- Input-list parity determines the two groups. Internal cache reordering for
  distortion registration must not change membership, weights or variances.
- The result is linear RGBA, with the same geometry and sample units as the
  master. Alpha is a **validity mask**, not fractional exposure coverage. A pixel
  is valid only if every RGB channel retains at least three samples in each half.
  Invalid RGB values are zero and alpha is zero; these are missing estimates,
  never evidence of a noiseless image.
- The existing aligned frame cache and tile reads are reused. Additional work is
  two half-group combinations and one output image; no second aligned cache or
  full half-stack images are created. Production callers remain opt-out.

## Calculation and limits

Both halves use the requested sigma-rejection settings. For each accepted set,
let `w` include frame weight and coverage and let `v` be the supplied relative
variance. Its weighted-mean variance proxy is

```
V = sum(w*w*v) / sum(w)^2
diagnostic = (mean(A) - mean(B)) * sqrt(V(full) / (V(A) + V(B)))
```

The variance identity holds for fixed weights, independent frames and linear
means with correct variance ratios. Sigma clipping selects samples from the data;
its conditional variance and the independently estimated half-group thresholds
make this scaling approximate. Synthetic Gaussian tests quantify that limitation;
they do not establish calibration for arbitrary camera data or rejection settings.

Differences in PSF, residual alignment, background and throughput can leak signal
into the half difference. Common fixed-pattern noise can disappear from it.
Different per-frame spatial correlations and signal-dependent shot noise are not
fully described by a single variance scalar. Thus the diagnostic must be inspected
and validated before being called a signal-free noise reference.

Do not attach a diagnostic to a different master (including a frequency coadd),
or use it after nonlinear denoising. Raw low-level diagnostic options remain
rejected by `DeepSkyWorkflow`; use its explicit noise-model option instead.

## Experimental workflow option

`denoise.noise-model "independent-luminance"` (default: `scene`) generates a
diagnostic during Sigma stacking. It requires `denoise.h "0"`, at least six
selected distinct exposures, and full diagnostic coverage in the finishing crop.
Three accepted samples per half is a validity minimum, not a quality guarantee.
Registered-frame high-pass luminance MAD squared supplies the relative variance
estimates, measured again after sensor correction if enabled.

The workflow writes `stack-noise.fits`, `stack-noise-inputs.txt`, and
`stack-noise-reference.txt` alongside `stack-linear.fits`. The manifest binds the
master, noise and input provenance by SHA-256. Provenance includes the recipe,
selected original indices, group assignments, input hashes, measured variances,
weights, calibration identities and sensor-model report identity. Keep the bundle
with the master. Missing or changed bundle members fail finish-only replay.

Finishing applies the same coverage crop and measured channel shifts to both
images. It does not independently detect stars in the noise realization. For this
conditional estimate the additive background fit is held fixed; its uncertainty
is excluded. Only luminance statistics use the diagnostic. Chroma statistics and
their scene-derived structural protection remain unchanged. The App retains the
association across finishing, save/reopen and switching back to scene estimates.
Enabling it on an older master without a bundle requires rebuilding the stack.

This option remains experimental: shared sensor patterns, PSF differences,
registration residuals and background-model uncertainty are not solved by the
association or hashes. Inspect the same native-scale regions before adoption.

## Verification

`StackNoiseDiagnosticTests.cpp` checks a known weighted contrast, fractional and
missing coverage, unequal variances, variance-unit invariance, signal cancellation,
Gaussian noise scaling, transient rejection, distortion-cache reordering and
invalid requests. With diagnostics enabled, master pixels, rejection maps and
counts must match the ordinary path exactly. The optional diagnostic does not
change accepted results or claim recovered optical detail.

## Component-specific denoising experiments

`MultiscaleDenoiseOptions.noiseReferenceScope` can restrict an explicitly supplied
noise realization to luminance with `MultiscaleNoiseReferenceScope::LuminanceOnly`.
The chroma thresholds **and the luminance support used to protect chroma** remain
scene-derived, so the original R-G and B-G solution is preserved up to Float32
rounding. A zero-noise luminance reference bypasses luminance suppression while
retaining that original chroma solution. With no reference, this option has no
effect; the default `AllComponents` retains the existing reference behavior.

This is also an engine API for controlled evaluation.
The same geometry, units, processing-stage and provenance requirements above
still apply. The M31 off-core quarter-stack assay found that replacing all three
component estimates could improve luminance agreement while worsening chroma;
its luminance-only follow-up is not full-image or cross-target acceptance.

`MultiscaleDenoiserTests.cpp` checks unchanged no-reference output, preserved
chroma including support-mask effects, luminance agreement with the all-component
reference, zero-noise behavior, protected pixels, alpha and invalid scopes.
