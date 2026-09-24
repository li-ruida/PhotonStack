# CLI Overview

PhotonStack includes a command-line interface for automation, scripting, and
reproducible processing workflows.

The CLI is intended to be a real product surface, not just an internal debug
tool.

## Goals

- support non-interactive image-processing workflows
- provide repeatable commands for testing and automation
- share the same core processing behavior as the macOS app
- expose machine-readable output for integration and tooling

## Experimental frequency coaddition

`photonstack frequency-stack --plan prepared.psfreq --output linear.fits --threads 2`
coadds registered scientific RGBA caches using explicit per-exposure PSF,
throughput, background and noise models. It preserves signed scientific values
and requires a new output path. The versioned input format and its limitations
are documented in [frequency coaddition](frequency-coaddition.md#prepared-input-cli).
Model preparation and saturation protection remain separate stages.

## Current Coverage

The CLI currently includes commands for:

- version reporting
- image and metadata inspection
- format conversion
- preview generation
- frame registration
- frame stacking
- master-frame generation
- calibration
- histogram and quality analysis
- normalization
- stretch and curves adjustments
- contrast, denoise, sharpen, and deconvolution operations
- color adjustments
- star detection, mask generation, and reduction
- FITS-oriented commands
- RAW-oriented commands on Apple-supported workflows
- line-based workflow execution with `photonstack run`

## Design Principles

- The CLI should not duplicate image-processing logic already owned by the
  shared engine.
- Public commands should remain automation-friendly and stable enough for
  scripts.
- Where practical, CLI parameters should align with concepts already exposed in
  the app.
- Structured output and deterministic error behavior matter as much as human-
  readable logs.

## Output and Automation

PhotonStack CLI is designed to support automation through:

- structured JSON output for result parsing
- stable error reporting
- progress events for long-running commands
- script-friendly command composition

These capabilities make the CLI useful for local batch processing, developer
regression checks, and future integration workflows.

## Relationship to the App

The CLI and the macOS app are expected to share the same processing engine.
That shared-core approach helps:

- reduce behavioral drift
- make regression testing easier
- keep advanced features available in both interactive and automated workflows

## Scope Notes

- The current public repository is macOS-first.
- Some RAW behavior depends on Apple imaging support available on the local
  system.
- The CLI surface is still evolving, but it already covers real end-to-end
  processing paths rather than only placeholder commands.

## Color FITS Light Frames

`stack` demosaics single-plane FITS tagged with `BAYERPAT` before alignment.
Supported patterns are RGGB, GRBG, GBRG, and BGGR; integer `XBAYROFF` and
`YBAYROFF` offsets are honored in stored row order. Bilinear interpolation keeps
the original dimensions and physical sample range. Untagged mono and RGB FITS
are unchanged. Use `--fits-debayer off` to explicitly stack sensor mosaics.
Calibrate sensor mosaics before demosaicing; ordinary FITS reads and master-frame
creation continue to use their existing raw decode behavior.

For lights taken under changing sky brightness, `--normalize-background on`
matches sampled per-channel median offsets to the first frame before rejection.
It does not rescale stellar flux or clip physical values. This assumes a similar
field of view and background-dominated images; it is not photometric calibration
or a substitute for flat fields or spatial gradient removal.

`stack --align similarity --refine-centroids on` optionally refines the coarse
stellar matches using elliptical Gaussian profiles with a local planar sky on
native pixels. It does not sharpen or resample the input before measuring its
centers. Incomplete coverage, clipped plateaus and poor profile fits are excluded;
the fit uses stars distributed across the image and rejects positional outliers.
If there are too few reliable, well-distributed stars, the coarse transform is
retained. Completion JSON reports `centroidRefinedFrames` and
`centroidRefinementFallbacks`; inspect these alongside ordinary alignment counts.
With `--align affine`, the same native centers are also used to compare similarity
and affine geometry on two alternating held-out folds. Affine is adopted only if
both folds improve and the combined clipped squared error falls by at least 5%
and 0.00001 square pixels. Spatial support and corner corrections are bounded;
otherwise the similarity result is retained. Completion JSON separates
`centroidAffineFrames` from `centroidSimilarityRetainedFrames` (successfully
refined frames retaining similarity). Neither count includes failed refinement.
The option defaults to `off`, requires similarity or affine alignment, and is
currently available through the CLI. A smaller fit residual alone is not an image-quality
guarantee: asymmetric profiles and spatially varying aberrations still require
independent validation and a rendered comparison.

`stack --alignment-reference image.fit` fixes the coordinate grid to an external
image without adding its pixels to the stack. It requires `--align` and accepts
the same dimensions, channels and color encoding as the science inputs. Frame
weights and frame counts refer only to the supplied science inputs. A single
science input is still registered. If background normalization is enabled, the
external reference supplies the per-channel sky offsets as well. Without this
option, reference selection is unchanged.

If that same file is also explicitly listed among the science inputs (including
through `--input` directory discovery), it contributes as a science input. The
reference option does not implicitly remove files from the supplied input list.

This allows disjoint subsets to be combined on the same grid without resampling
their completed stacks. For independent noise checks, exclude the reference from
both science subsets; common coordinate estimates and any common calibration
errors still limit what a split-stack agreement can prove.

```bash
photonstack stack --input lights --output M31-linear.fits --method sigma \
  --align similarity --minimum-matches 30 --normalize-background on
photonstack fits convert --input light.fit --output light-rgb.fits \
  --fits-values scientific --fits-debayer on
photonstack stretch --input M31-linear.fits --output M31-stretched.fits --auto
```

FITS conversion requires explicit `--fits-debayer on`; its default preserves the
existing sensor-sample behavior. Keep linear FITS as the processing master.
Preview and registration also decode tagged Bayer lights into RGB. If a prior
calibration/export tool strips `BAYERPAT`, restore the correct sensor metadata
before demosaicing; this path does not guess a pattern from untagged gray images.

Color-preserving stretch measures RGB ratios relative to its selected black
point. This also supports signed, background-subtracted FITS without amplifying
near-zero luminance into saturated chroma noise.

## See Also

- [`README.md`](../README.md)
- [`docs/features.md`](features.md)
- [`docs/image-pipeline.md`](image-pipeline.md)

## Linear Deep Sky Development

`color-calibrate --input master.fits --reference reference.fits` estimates RGB
gains relative to a supplied **linear RGB** reference. It reports JSON and does
not write either image. Native centroid matching selects stars; radius-8 aperture
photometry subtracts a local plane from the radius-11–15 sky annulus. Accepted
measurements are separated by at least 32 pixels in both images. Alternating
spatially sorted stars form separate training and validation groups (at least
12 each). The green gain is fixed to 1. A held-out 90th-percentile absolute log
color-ratio error above 0.2 rejects the calibration. This checks whether a single
diagonal gain describes the matched stars; it does not prove absolute color
accuracy or that faint extended emission has the same response.

Use `--reference-flip-y on` when the reference has the opposite vertical FITS
orientation. This reverses rows exactly in memory; it does not interpolate the
photometry. `--input-saturation` and `--reference-saturation` specify positive
limits in each image's own scientific units. No saturation limit is inferred by
default, so supply the appropriate limits for masters containing clipped stars.
The fixed aperture is intended for similarly sampled, compact stars; unsuitable
sampling, saturated data, gradients, crowded fields or a nonlinear reference can
invalidate color transfer. FITS does not reliably declare whether its values
have already been stretched: callers must supply linear masters, not exported
display images or CFA mosaics. This is reference-relative calibration, not SPCC
or a catalog-based white point.

Apply returned gains with `develop --stellar-balance off --red-gain <r>
--green-gain 1 --blue-gain <b>`. Review the display curve separately: relative
color calibration does not set exposure, background or contrast. This command
is currently a CLI facility; it does not change the App's default white balance.

`develop` reads scientific linear RGB values (and debayers tagged CFA FITS),
then applies statistical stellar white balance, local stellar exposure control,
and a luminance-based display transform with smooth shadows and highlight
chroma compression. It writes a display-referred image; keep the linear master.
Run background gradient extraction before development. Already stretched sRGB
inputs are rejected rather than being stretched twice.

Near the display gamut boundary, a smooth chroma shoulder preserves mapped
luminance and RGB chroma direction instead of placing colored highlights
directly on a black or white channel endpoint. Colors within 80% of the
available gamut are unchanged by this step. This reduces endpoint flattening;
it cannot recover saturated input data or guarantee headroom for every extreme
input after output quantization.

Display-mode `denoise` limits the whole RGB adjustment near gamut boundaries,
so `--amount 0 --chroma-amount 0.45` preserves weighted luminance even around
bright colored sources. It reserves one 16-bit code of endpoint headroom for
new adjustments; existing clipped input cannot be recovered. Scientific mode
with unclamped output retains its physical values and does not apply this
display limiter.

For optional green-cast styling of an already balanced display image:

```sh
photonstack color remove-green --input developed.tiff --output styled.tiff \
  --green-method average-neutral --amount 0.65 --green-threshold 0 \
  --preserve-lightness on
```

`--green-method background` retains the existing background-weighted behavior.
The opt-in `average-neutral` mode blends green toward the mean of red and blue
where green exceeds that mean and the specified threshold; it ignores
`--background-limit`. `--preserve-lightness on` decodes sRGB, restores the
original linear-sRGB Y, then compresses chroma smoothly if needed to fit the
display gamut. Preserving Y also preserves CIE L* before quantization. This is
a pointwise operation, with no smoothing or recovered spatial detail. It is
not identical to a Lab a*/b*-preserving conversion.

Both new options require bounded sRGB display samples. The CLI rejects FITS
input or output when either is enabled; keep the linear master unchanged.
Defaults remain `background` and `off`, preserving saved processing behavior.
Neutral and non-green pixels remain unchanged. This aesthetic operation can
alter legitimate object colors; it is not a replacement for background or
photometric color calibration. The average-neutral rule is also documented in
[Siril's color tools](https://siril.readthedocs.io/en/stable/processing/colors.html).

For a cooler diffuse continuum with no increase in display channel values:

```sh
photonstack color cool-continuum --input styled.tiff --output cooled.tiff \
  --amount 0.06 --radius 32
```

This optional display operation estimates a smooth continuum, then subtracts
the same pedestal from encoded red and green.
Blue and covered alpha samples remain unchanged. It deliberately lowers diffuse
display luminance; it is not a physical white balance or a resolution correction.
It does not rescale compact residuals to make room for added blue highlights.
The subtraction fades out on dark and near-white continuum and is limited to
80% of the smaller red/green sample to retain a positive floor.

`--amount` accepts finite values in `[0,0.12]`; zero preserves covered samples.
`--radius` accepts integers in `[4,64]` and defaults to 32. Inputs must be bounded
sRGB RGB/RGBA. FITS input/output and output aliases of the input are rejected.
`--estimator source-excluded` is the default: it detects compact sources and
continues the surrounding background through their masks before smoothing.
`--estimator opening` instead uses a grayscale opening with a fixed 13-by-13
pixel square, followed by the same smoothing. It suppresses compact positive
peaks in the continuum estimate without a discrete source-detection threshold;
it does not apply that opening directly to the output image. The two estimators
can differ around crowded sources, broad stars and curved galaxy backgrounds.
Neither guarantees unchanged stellar aperture flux or color. The smoothing
radius does not change the opening's fixed footprint.
Keep the linear master for scientific measurements. A locally constant
continuum preserves stellar RGB residuals when the floor guard is inactive;
curved backgrounds, source estimation and display encoding can still change
measured apertures. Inspect actual colors, faint structures and highlights.
This command is not inserted into existing processing recipes automatically.

```sh
photonstack develop --input background-linear.fits --output developed.tiff \
  --stellar-balance on --brightness 1.5 --background 0.055 \
  --saturation 0.65 --star-exposure 0.25 --red-gain 0.92 --blue-gain 1.12
```

`--brightness` is positive (up to 20); `--tone-scale` optionally specifies the
half-response in input physical units, before division by brightness. Zero
selects a scene-based estimate. `--background` is in [0, 0.5), `--saturation`
in [0, 3], and RGB gain multipliers in (0, 20]. `--star-exposure` is a linear
stellar signal multiplier in [0.05, 1]; 1 leaves stars unchanged. Compactness
checks protect broad nuclei; overlapping stellar masks are not multiplied.
`--star-peak-threshold` in [0, 1] optionally restricts that adjustment to brighter
stars. It compares the local-background-subtracted stellar peak after mapping
through the selected display curve, and smoothly increases attenuation from
zero at the threshold to the requested strength at white. Zero preserves the
original all-detected-star behavior; 1 disables attenuation. For example,
`--star-exposure 0.5 --star-peak-threshold 0.75` reduces bright compact sources
without selecting sources below that display threshold. Pixels overlapping a
selected bright source's region can still change. In this selective mode, the
source profile determines the taper radius; nearby background controls source
selection and extent, while a more distant annulus estimates the broad wings.
The threshold is independent of FITS physical units and available through
the CLI and macOS development control.
No artificial stars or galaxy features are generated.

The JSON result reports the number of calibration and adjusted stars, applied
channel gains, estimated sky levels, and the effective tone scale. Calibration
needs at least eight usable stars. To use manual gains instead, pass
`--stellar-balance off`. This is statistical color balancing, **not** catalog
photometry or SPCC. Keep `--star-exposure 1` for measurements of stellar flux.

The macOS action bar exposes **Deep Sky Development / 深空显影**, including
brightness, background, stellar exposure, bright-star threshold, saturation,
and red/blue adjustments. A threshold of zero selects all detected stars for
the exposure adjustment; larger values restrict it to brighter stars, and one
leaves stars unchanged. Old operations without a recorded threshold retain zero.
Settings are retained in the edit operation and used during replay; existing
auto-stretch operations retain their previous behavior.
When reproducing a CLI recipe in the App, set brightness explicitly: the CLI
default is 1.0 and the App default is 1.5. Brightness divides the tone scale,
so a fixed tone-scale value alone does not specify the complete transfer.

**Color cast / 色偏调整** applies the separate display-space green adjustment.
It offers global neutralization or background weighting, amount, green allowance,
and optional lightness preservation. Start from a developed image; the control is
disabled for FITS input. Each operation records the method and all settings, which
can be edited in processing history. Replay keeps this display step in 16-bit TIFF,
including during full-resolution export. Legacy green-removal records keep their
original background weighting and do not enable lightness preservation implicitly.

For the separate display-space `stars reduce` command, `--max-stars` controls
the detection limit. Use modest settings and inspect stellar profiles before
accepting morphological reduction.

### FITS detail-preserving reconstruction

`convert`, `fits convert`, and `stack` accept `--fits-demosaic bilinear|malvar|menon|ratio`.
The default remains `bilinear`. The option is used only when FITS debayering is
on and the input is a tagged, single-channel Bayer image. `malvar` uses the
5×5 gradient-corrected filters from
[Malvar, He and Cutler (2004)](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/Demosaicing_ICASSP04.pdf).
Measured sensor samples and signed scientific values are preserved. Borders and
neighbourhoods containing invalid filter samples fall back to finite-neighbour
bilinear interpolation. Calibration masters remain undecoded sensor mosaics.

`menon` is an optional directional reconstruction with a posteriori selection
and colour-difference refinement (DDFAPD, Menon et al. 2007). The C++ adaptation
is based on [Colour - Demosaicing 0.2.7](https://colour-demosaicing.readthedocs.io/en/develop/_modules/colour_demosaicing/bayer/demosaicing/menon2007.html)
and retains its BSD-3-Clause notice in `third_party/colour-demosaicing-LICENSE.txt`.
It mirrors boundary samples and falls back locally when a reconstructed value
is non-finite. It preserves measured CFA sites and signed units. Compare star
shape, colour artifacts and noise on representative data before selecting it;
its use does not guarantee better results than another method.

`ratio` is an experimental ratio-guided reconstruction with fixed direction
weights, signed-sample fallback, and a low-frequency correction relative to
Malvar. Measured CFA samples remain unchanged. It can reduce interpolation
colour artifacts, but does not guarantee increased resolution or replace
noise reduction. It is a separate implementation, not the RCD algorithm.
The deep-sky recipe also accepts `decode.demosaic "ratio"`; existing defaults
remain unchanged. Compare native-scale results before selecting this method.


`stack --interpolation bilinear|bicubic` selects the registration resampler.
The default preserves existing behavior (bilinear for similarity, affine and
distortion; rounded nearest-pixel translation). `bicubic` uses Catmull-Rom
interpolation clamped to the central 2×2 sample range to limit overshoot. It
supports fractional translation too. Image boundaries and incomplete coverage
fall back to straight-alpha bilinear sampling; negative lobes do not extrapolate
through masked pixels. Both choices preserve scientific sample units.

For example, compare the same light frames with:

```sh
photonstack stack --input ./lights --output ./stack-detail.fits \
  --method sigma --align similarity --normalize-background on \
  --fits-demosaic malvar --interpolation bicubic
```

Sharper interpolation can retain more noise as well as detail. Compare native
pixel star profiles, shape, faint structure and artifacts before selecting a
reconstruction method for a dataset; interpolation alone is not a resolution or
signal-to-noise guarantee.

`develop --tone-curve asinh` offers a linked inverse-hyperbolic-sine stretch
with a smooth highlight shoulder. `--tone-scale` controls the linear-to-log
transition; `--white-point` controls highlight normalization in the input's
physical units. Zero selects scene estimates for either value. The default CLI
curve remains `rational`; old saved development operations retain that curve.
The Develop panel starts new edits with `asinh` and full stellar exposure to
preserve compact star profiles. This is a display transformation, not sharpening
or restoration of missing astronomical detail. See also the
[Siril stretching documentation](https://siril.readthedocs.io/en/stable/processing/stretching.html)
for the role of asinh stretching in a linear-image workflow.

### Protecting an extended target during background fitting

`background --model polynomial` fits a robust quadratic surface independently
per color channel to local sky medians. It supports subtraction and preserves
signed values in FITS outputs. `--grid-x` and `--grid-y` select the sampling grid
(default 32×48). Samples with incomplete coverage are excluded.

Repeat `--exclude-ellipse x,y,major,minor,angle` to protect extended targets.
Centers and **semiaxes** use input pixel coordinates; the angle is in degrees
from the positive x axis toward positive y. The ellipse only excludes sky-fit
samples: the fitted background is still subtracted smoothly over the whole
image. Exclusions require the polynomial model. A fit fails if too few or
poorly distributed sky samples remain.

```sh
photonstack background --input stack.fits --output background-corrected.fits \
  --model polynomial --exclude-ellipse 986,1776,1450,650,55 \
  --preserve-brightness off
```

The example coordinates must be adapted to the target and crop. Inspect both
the faint outer structure and the remaining sky: a smooth background alone does
not prove that diffuse astronomical signal was preserved.

For Malvar and Menon interpolation, `--fits-cfa-gains r,g,b` optionally normalizes the
relative sensor channel response before cross-channel gradient interpolation,
then reverses those gains in the reconstructed result. It preserves measured
sensor samples and output physical units; it does not replace final color
calibration. The default is `1,1,1`. This is an experimental control: assess
colored star edges on a matched pilot before applying it to a complete dataset.
Do not automatically reuse the final white-balance gains here. A gain set can
improve reconstruction of one source color while increasing error for another,
and it can redistribute noise between channels. Unchanged measured CFA samples
and a narrower fitted star core do not establish better reconstructed color or
detail; include cool and emission-like sources as well as neutral stars in the
comparison.

### Explicit stack weights

`stack --frame-weights w1,w2,...` assigns one finite, positive relative weight
per resolved input file. Input order is positional inputs followed by the
sorted directory inputs when `--input` is also used. Specify an explicit file
list when recording weights for reproducible runs. The number of weights must
match the input count. Omit the option for the previous equal-weight behavior.

Weights multiply pixel coverage in average and robust combinations. The
`weighted` method also multiplies its existing automatic quality score.
Coverage in the result is the weighted fraction of available observations.
Weights follow their original input even when distortion registration chooses
the middle frame as its reference. Multiplying every weight by the same positive
constant does not change the result.

For calibrated images with comparable source throughput, inverse sky variance
is a useful candidate for extended faint-signal weighting; a PSF penalty may
instead favor compact sources. Neither choice guarantees a better result:
measure flux-normalized noise and star profiles on the same input set before
using a weighting recipe. The CLI does not silently infer or apply such weights.

### Small PSF covariance correction

`psf-match --input linear.fits --output corrected.fits --cov-xx 1.16
--cov-yy 0.92 --cov-xy 0.03 --strength 0.7` applies an experimental, spatially
invariant 3 by 3 signed kernel. Measure covariance in the input's native pixel
grid, using one set of isolated stars, and validate on separate stars across
the field. The three covariance components are in squared pixels. The target
is a circular Gaussian with the same model area as the measured PSF.

This finite-difference approximation transfers a small amount of width between
directions; it is not a general deconvolution or proof of recovered resolution.
The same kernel is applied to all color channels. FITS output preserves negative
samples and physical units. The maximum absolute covariance-change eigenvalue
is limited to 0.5 squared pixels. Image borders and neighborhoods with incomplete
coverage retain the original pixels. Zero-coverage pixels remain masked.

The command reports the kernel and its predicted gain for uncorrelated white
noise. Real stacked noise is correlated and must be measured separately. Check
stellar wings, dark lobes, background noise, and extended-object structure before
accepting a result. Parameters are explicit and are not enabled by default.

### Reconstruction validity protection

`protect-reconstruction --input reconstructed.fits --reference direct.fits
--mask weights.fits --output protected.fits --background-sigma 8 --amount 1`
replaces fine-scale differences in selected regions of a linear reconstruction
with those from a direct coadd. Inputs must already have the same pixel grid,
color channels and photometric scale. This command does not register or normalize
them. All input and output files must be FITS; the output cannot overwrite an input.

The grayscale mask contains weights in [0,1], typically derived from persistent
native-exposure clipping or another measured validity failure. Mask generation
is external to this command. With `D = reference - input`, the adjustment is
`amount * mask * (D - Gaussian(D, background-sigma))`. The Gaussian uses reflected
borders and four-sigma support. Incomplete coverage is handled by a normalized
convolution. The reference must cover each active protection pixel and should
contain valid direct-coadd data throughout its surrounding Gaussian support.

`background-sigma` is in pixels, between 0.25 and 64; `amount` is between 0 and 1.
Pixels outside the effective mask retain their input values exactly. Float32
scientific values, including negative values and values above one, are preserved
without display clipping. This operation cannot recover saturated flux, and can
broaden protected sources. Inspect bright-source wings, mask transitions and
extended structure before accepting the result. It is optional and is not
automatically applied by `stack`.

### Protected structure contrast

For a stretched RGB image, use `local-contrast --input developed.tiff --output
structure.tiff --protect-structure on --fine-radius 6 --radius 48 --amount 1.2`
to enhance intermediate-scale luminance structure. This optional mode uses a
5 by 5 median estimate, two Gaussian scales, and compact-source exclusion.
Stellar holes in the structure estimate use a smooth harmonic continuation;
the original stars and fine texture remain the base of the output. Smooth
background changes extend underneath foreground stars to avoid dark mask spots.
Dark sky and highlights receive reduced or zero adjustment. RGB channel differences
are preserved by adding the same luminance correction to each channel, bounded
by the available gamut; large corrections ease into that limit instead of
abruptly clipping, which reduces flat highlights. This does not guarantee
constant perceived saturation.
New structure adjustments reserve one 16-bit code of endpoint headroom;
existing samples closer to the endpoints are not changed solely to enforce it.
Optional `--star-chroma 0.7` reduces color strength inside the stellar mask while
preserving display-domain weighted luminance. It defaults to 1 (unchanged) and requires protected mode.
This is a display styling adjustment, not photometric calibration or removal of
an optical aberration. The underlying linear master is not changed.

`fine-radius` must be positive and less than `radius` (at most 64 pixels).
Input samples must be in [0,1]; FITS output is rejected in this mode. It is a
display operation and does not recover optical resolution. Mask thresholds
assume an ordinary stretched deep-sky image; inspect crowded fields, broad
bright stars, nuclei, and mask transitions at native size. The mode is disabled
by default, preserving existing local-contrast behavior. This is conceptually
consistent with separate scale and star-mask processing described in the
[Siril mask documentation](https://siril.readthedocs.io/en/latest/processing/masks.html);
PhotonStack's implementation does not use Siril code.

Protected mode also accepts an optional curve on its estimated diffuse continuum:

```sh
photonstack local-contrast --input developed.tiff --output continuum.tiff \
  --protect-structure on --fine-radius 3 --radius 12 --amount 0.65 \
  --continuum-curve 0:0,0.12:0.12,0.18:0.18,0.4:0.47,0.6:0.7,0.8:0.86,1:1
```

`--continuum-curve` uses the existing monotone Hermite curve interpolation on the
coarse, compact-source-excluded luminance estimate. Points must have unique inputs,
nondecreasing outputs, and endpoints `0:0` and `1:1`. Both input and output must be
display images; FITS is rejected. Omit the option to retain the previous operation.
An identity curve is an exact bypass of the additional tone step.

For original continuum `b` and mapped continuum `t`, recombination is
`t + k * (pixel - b)`, with a common RGB scale
`k = min(1, t/b, (1-t)/(1-b))`; undefined endpoint terms are omitted. This retains
the residual profile when the continuum is locally constant and keeps bounded
input colors within the display gamut. It changes stellar flux and perceived
saturation, so it is not photometric processing. A spatially varying or biased
continuum can change apparent star profiles; inspect the actual image, especially
crowded regions and galaxy nuclei. Setting `--amount 0` disables the band contrast
increment while retaining the optional continuum curve.

The App's **Diffuse tone / 星系层次** control exposes tone strength and detail
contrast for developed images. Advanced controls set both radii and star color
retention. Tone strength interpolates a fixed curve toward identity; operation
history stores the actual curve points and processing parameters. Preview and
project replay use TIFF intermediates. History edits, undo/redo, and 16-bit export
retain these parameters. FITS input is disabled until developed, and incomplete
or invalid saved parameters fail explicitly. Existing local-contrast operations
without this mode retain their previous behavior.

### Source-protected nonlocal denoising

`denoise-nonlocal --input linear-rgb.fits --blend-map weights.fits --output new.fits --h 1.5`
provides an explicit scientific denoising step. It compares 7×7 luminance patches
in an 11×11 search neighborhood and applies the same nonnegative averaging
weights to RGB. `--h` is required, in the input's weighted luminance units; zero
is an exact bypass. Optional `--red-weight`, `--green-weight`, and `--blue-weight`
default to 0.2126, 0.7152, and 0.0722. These coefficients are not normalized
internally, so changing their scale also changes the appropriate `h`.

`--mode conservative-rgb` selects weighted RGB patch distances and symmetric
pixel transfers. Each transfer adds and subtracts the same RGB amount, preserving
channel sums up to floating-point rounding. Both endpoints must have positive
blend weights, so protected pixels neither send nor receive a transfer. The
default `--mode luminance` retains the luminance-guided averaging behavior.
The RGB distance is `sum(w) * sum(w[c] * delta[c]^2)` averaged over the patch;
its noise scale generally differs from the luminance distance. Calibrate `h`
for the chosen mode; reusing a value does not ensure equal smoothing strength.
Global channel-sum conservation does not guarantee aperture photometry,
unchanged colors at individual sources, or retained spatial detail.

The grayscale blend map must share the input grid, have values in [0,1], and
have complete coverage. Zero preserves a pixel exactly, including source cores;
one applies the full adjustment. Both images require finite samples and complete
coverage; the image must originate as RGB FITS, rather than monochrome or CFA.
The output retains scientific values and is never display-clipped. Existing
output files and symlinks are rejected. Input FITS must already be linear;
FITS metadata alone cannot certify its processing history.

The CLI does not estimate noise, build source masks, identify diffuse structures,
or choose the blend map. These are explicit upstream responsibilities. Faint
structures can be attenuated even when prediction error decreases, so compare
structure retention and source photometry in addition to noise. This optional
command does not alter the existing `denoise` command or App defaults.

## Gamut-preserving luminance curves

`curves --channel luminance --preserve-gamut on` maps each pixel's weighted
RGB luminance through the selected curve, then scales its RGB differences
from luminance together to stay within `[0,1]`. It avoids independent channel
clipping and follows a lifted black point. It requires bounded visible input
samples; covered invalid color remains an error and masked pixels are cleared.
The option is off by default and is only valid with the luminance channel.

```sh
photonstack curves --input developed.tiff --output toned.tiff \
  --channel luminance --preserve-gamut on \
  --points '0:0,0.12:0.12,0.18:0.18,0.4:0.47,0.6:0.7,0.8:0.86,1:1'
```

This is a pointwise display adjustment, not star separation or sharpening.
A monotone curve gives a monotone scalar luminance transfer, but its changing
slope can alter displayed star profiles and noise contrast. Gamut compression
preserves the direction of encoded RGB differences, not physical photometry
or perceptual hue. Validate the complete export, including later color steps,
when comparing results.
### Display grid suppression

`suppress-grid --input display.tiff --output reduced.tiff --amount 1`

Applies a fixed, separable narrow-band filter to encoded sRGB display pixels.
TIFF and PNG are accepted; output is 16-bit, and an existing destination
(including symbolic links) is rejected. FITS and camera RAW are not accepted.
`--amount` is in `[0,1]` and defaults to `1`; `0` bypasses the filter before
normal codec export. This command is optional and is not part of default stacking.

Before the gamut guard, the full-strength response is
`(1 - sin(pi*fx)^128) * (1 - sin(pi*fy)^128)`, with frequencies in cycles/pixel.
The 129-tap filters reflect at half-pixel image boundaries. A continuous shared
RGB factor reduces corrections near black or white instead of clipping channels
independently. A correction is applied only when the full 129×129 reflected
support has complete coverage; other covered pixels and their alpha remain
unchanged, while masked pixels are cleared.

The filter can reduce fine horizontal/vertical grid structure, but attenuates
real detail in the same frequency band. It does not increase scientific
resolution, guarantee aperture flux, or recover missing color information.
Compare native-size dust lanes and star wings before retaining an output.
# Shared deep-sky recipes

The `deep-sky` command runs the same assessment, selection and processing workflow as the desktop's **Deep Sky Assessment & Stack** window. See [Shared deep-sky workflow](deep-sky-recipe.md) for recipe import/export, diagnostics and output retention.

### Frame review preview

`photonstack review-preview --input light.fit --output new-preview.tiff` produces an original-resolution display preview for screening. FITS use display normalization, debayering on, and Malvar interpolation by default; `--fits-debayer on|off` and `--fits-demosaic bilinear|malvar|menon|ratio` override these choices. The output is an 8-bit sRGB TIFF stretched to a target background of 0.18. Existing output files are rejected. This display image is not a scientific stacking input.
