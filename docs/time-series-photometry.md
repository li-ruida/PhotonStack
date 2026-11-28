# Time-series photometry

**Post-processing → Advanced → Time-series photometry** measures a target against a fixed ensemble of 2–16 comparison stars. Choose at least three mono/RGB linear FITS frames, click the target and comparison stars in the reference preview, confirm the common grid and filter, enter the effective gain and aperture settings, then select **Measure & save**. Results include an error-bar chart, comparison stability estimates, rejected-frame reasons and CSV exports. Saved App sessions can be reopened with the original selection and preview.

Images must already be calibrated, demosaiced where applicable, and aligned on a common pixel grid. This tool does not perform those operations. Dimensions, channel count and available WCS are checked; absent WCS cannot prove alignment. Each star is recentered within the configured maximum shift. Use isolated stars: measurement regions must not overlap. A fixed comparison ensemble avoids introducing jumps when a comparison disappears; the entire frame is rejected if any chosen star fails.

## Measurement and uncertainty

The existing stellar photometry engine uses fractional aperture coverage, a robust local planar sky model and an annular background estimate. The selected R/G/B channel is measured directly, without hidden luminance mixing. The differential signal is target flux divided by the sum of comparison fluxes, normalized to the median accepted ratio. CSV also contains differential instrumental magnitude, `-2.5 log10(target/ensemble)`, which is not a catalog or absolute magnitude.

Effective gain means electrons per current sample unit. It is not a camera gain setting or ISO. Scaling or averaging images changes effective gain. Background variance includes the empirical sky and sky-fit uncertainty; source photon variance is `Σ w² max(sample − fitted_sky, 0) / effective_gain`. Target and comparison variances propagate into the ratio and magnitude errors. The estimate assumes independent samples and independent measurement regions. It excludes scintillation, flat-field systematics, resampling covariance and uncertainty in the median normalization. Data without known effective gain cannot substantiate calibrated error bars.

Comparison leave-one-out RMS measures each comparison relative to the remaining ensemble. Review these values and the images; no automatic variable-star identification is claimed. Unknown saturation thresholds are explicitly indicated. Cloud extinction is only canceled to the extent target and comparison stars respond together.

## Time convention

Exposure start comes from `DATE-OBS` or `MJD-OBS`, with duration from `EXPTIME` or `EXPOSURE`. Both fields, when present, must agree. `TIMESYS` must be UTC or absent (assumed UTC). Output is the UTC Julian Date of the exposure midpoint. There is no barycentric or heliocentric correction, observatory clock correction, or standard-filter transformation.

The embedded [IERS leap-second table](https://hpiers.obspm.fr/iers/bul/bulc/Leap_Second.dat), updated through Bulletin C 72, bounds accepted timing to 1972-01-01 through 2027-06-27. Exposures touching a leap-second day and timestamps outside this verified range are rejected; they require an explicitly leap-aware timing conversion. Leap-second samples are not silently mapped onto ordinary 86400-second days. UTC JD is a coordinate, not a uniform elapsed-seconds scale across leap seconds.

## CLI recipe

```sh
photonstack time-series --recipe sequence.txt --output-dir new-photometry
```

```text
PhotonStack-TimeSeries-1
aligned "true"
filter "L"
channel "mono"
effective-gain "1.5"
aperture "8"
sky-inner "11"
sky-outer "15"
saturation "50000"
min-snr "5"
max-shift "3"
target "target" 48 48
comparison "c1" 136 48
comparison "c2" 48 136
frame "/absolute/frame1.fits"
frame "/absolute/frame2.fits"
frame "/absolute/frame3.fits" "2026-09-10T00:02:00Z" 60
```

Coordinates are zero-based from the top left. The report records any explicit UTC start and duration used in place of header timing. Strings use C++ quoted-string escaping. Files and star IDs must be unique. At each exposure midpoint, the first frame that passes measurement supplies the light-curve point. Each rejected frame retains its reason in the report. Choose a new output directory.

Outputs are `lightcurve.csv`, `measurements.csv`, the exact `recipe.txt`, `report.json` and `report.sha256`. App runs also save `session.json`, whose generated recipe must match the report. Input, recipe and product SHA-256 digests are verified on reopening. Digests detect accidental changes; they are not digital signatures. Sources stay read-only.

## Local validation

Independent synthetic Gaussian stars test known 3% target variation under changing transparency, exposure and subpixel position. Tests check recovered ratios, analytic source-noise estimates, exact uncertainty propagation, UTC leap-day midnight, explicit timing, rejected saturation/NaNs/filter conflicts, leap-second boundaries, duplicate inputs/times, existing output rejection and changed products. App service integration exercises preview, selection, actual CLI measurement, reopening and modified CSV/report rejection. Synthetic recovery is not an absolute photometric calibration or evidence of performance on an unknown variable star.
