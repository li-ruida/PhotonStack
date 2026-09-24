# Experimental CFA drizzle accumulator

`CfaDrizzleAccumulator` reconstructs RGB directly from one-channel Bayer sensor
samples and externally supplied affine registration transforms. It is an engine
building block; the existing `drizzle` command and App workflow do not yet select it.

Each sensor pixel contributes only to its own CFA color. The input square, shrunk
by `pixfrac`, is transformed into a quadrilateral. Exact polygon overlap with
output pixel squares determines its deposited weight. Red, green and blue have
separate weighted sums and coverage maps. Normalizing each channel by its own
coverage preserves a constant surface brightness; it does not guarantee exact
aperture photometry under spatially varying coverage.

Coordinates use integer pixel centers and the stored sensor row order. CFA
offsets follow `FitsCodec`'s `XBAYROFF` / `YBAYROFF` convention. For reference
position `q`, output position is `scale * (q - origin + 0.5) - 0.5`. An output crop
therefore changes the canvas origin without changing the sensor's Bayer phase.
Affine transforms may include rotation, shear or reflection. Nonlinear distortion
maps are not supported.

Start exploratory CFA comparisons at `scale = 1`, `pixfrac = 1`, as recommended
in the [Siril drizzle documentation](https://siril.readthedocs.io/en/latest/preprocessing/drizzle.html).
Smaller footprints and larger scales require more coverage and can increase
noise. The implementation is original polygon clipping code, not copied from Siril.

The caller supplies calibrated sensor samples, optional validity weights, a
positive frame weight (at most `1e12`), and per-channel additive background and multiplicative
gain. Scientific negative values are retained. The caller is responsible for
FITS metadata, registration, calibration and outlier rejection. No demosaicing,
hole filling, sharpening or rejection takes place in this accumulator.
Uncovered channels are NaN; output alpha is one only when all three colors are
covered. The coverage map must be checked before export or further processing.

The accumulator is not thread-safe. Accumulate separate groups independently,
then combine their weighted sums and weights to form a full reconstruction.
Weights describe deposited samples, not independent exposure counts or a noise
variance estimate. Partial sensor validity outside `(0, 1]`, nonfinite sensor
values and nonfinite validity values do not contribute. Calibration that would
exceed the Float32 scientific range is rejected before accumulation.

`photonstack_cfa_drizzle_tests` checks all Bayer patterns and signed offsets,
constant-color reconstruction, missing-color behavior, negative values,
half-pixel geometry, transformed drop area and weighted-count conservation,
ROI equivalence, invalid samples and invalid geometry. Scientific image quality
still requires comparisons of the same exposures, native-scale stellar profiles,
color, coverage, noise, and rendered artifacts.
