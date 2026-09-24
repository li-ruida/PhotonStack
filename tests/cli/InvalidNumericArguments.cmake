if(NOT DEFINED CLI)
  message(FATAL_ERROR "CLI executable path is required")
endif()

function(expect_invalid_numeric name expected_message)
  execute_process(
    COMMAND "${CLI}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(result EQUAL 0)
    message(FATAL_ERROR "${name}: command unexpectedly succeeded")
  endif()

  set(combined "${stdout}\n${stderr}")
  string(FIND "${combined}" "${expected_message}" message_position)
  if(message_position EQUAL -1)
    message(FATAL_ERROR
      "${name}: expected '${expected_message}' in output, got:\n${combined}"
    )
  endif()
endfunction()

expect_invalid_numeric(green_invalid_method "--green-method requires background or average-neutral"
  color remove-green --green-method other)
expect_invalid_numeric(green_invalid_preserve "--preserve-lightness requires on or off"
  color remove-green --preserve-lightness maybe)
expect_invalid_numeric(green_style_fits_input "Green display styling requires non-FITS input and output"
  color remove-green --input missing.fits --output output.tiff --preserve-lightness on)
expect_invalid_numeric(green_style_fits_output "Green display styling requires non-FITS input and output"
  color remove-green --input missing.tiff --output output.fits --green-method average-neutral)

expect_invalid_numeric(protection_nonfinite_sigma "--background-sigma requires a finite number"
  protect-reconstruction --background-sigma nan)
expect_invalid_numeric(protection_out_of_range "Protection amount must be in [0,1]"
  protect-reconstruction --amount 2)
expect_invalid_numeric(protection_requires_fits "protect-reconstruction requires FITS"
  protect-reconstruction --input a.png --reference b.fits --mask c.fits --output d.fits)
expect_invalid_numeric(protection_input_output_conflict "Protection output must not overwrite any input"
  protect-reconstruction --input a.fits --reference b.fits --mask c.fits --output a.fits)

expect_invalid_numeric(
  color_nonfinite_saturation
  "--input-saturation requires a finite positive number"
  color-calibrate --input missing.fits --reference missing.fits --input-saturation nan
)
expect_invalid_numeric(
  color_rejects_display_reference
  "color-calibrate requires --input <linear RGB FITS> --reference <linear RGB FITS>"
  color-calibrate --input missing.fits --reference image.png
)
expect_invalid_numeric(
  color_rejects_invalid_flip
  "--reference-flip-y must be on or off"
  color-calibrate --reference-flip-y maybe
)

# Mono FITS expands to RGBA in the codec. Calibration must check the source
# planes, otherwise duplicated grayscale channels could masquerade as RGB.
set(gray_header "")
foreach(card "SIMPLE  =                    T" "BITPIX  =                    8"
             "NAXIS   =                    2" "NAXIS1  =                    8"
             "NAXIS2  =                    8" "END")
  string(LENGTH "${card}" card_length)
  math(EXPR card_padding "80 - ${card_length}")
  string(REPEAT " " ${card_padding} padding)
  string(APPEND gray_header "${card}${padding}")
endforeach()
string(LENGTH "${gray_header}" header_length)
math(EXPR header_padding "2880 - ${header_length}")
string(REPEAT " " ${header_padding} padding)
string(REPEAT " " 2880 gray_data)
set(gray_fixture "${CMAKE_CURRENT_BINARY_DIR}/color-gray-cli-regression.fits")
file(WRITE "${gray_fixture}" "${gray_header}${padding}${gray_data}")
expect_invalid_numeric(
  color_rejects_expanded_grayscale
  "Color calibration requires linear RGB, not CFA or grayscale"
  color-calibrate --input "${gray_fixture}" --reference "${gray_fixture}"
)
file(REMOVE "${gray_fixture}")

expect_invalid_numeric(
  develop_nonfinite_star_peak_threshold
  "--star-peak-threshold requires a finite number"
  develop --input missing.fits --output output.tiff --star-peak-threshold nan
)

expect_invalid_numeric(
  stack_missing_reference_path
  "--alignment-reference requires an image path"
  stack --alignment-reference --output output.fits missing.fits
)
expect_invalid_numeric(
  stack_invalid_centroid_refinement
  "--refine-centroids requires on or off"
  stack --refine-centroids yes --output output.fits missing.fits
)

expect_invalid_numeric(
  structure_rejects_nonfinite_star_chroma
  "--star-chroma requires a number in [0,1]"
  local-contrast --input missing.tiff --output output.tiff --star-chroma nan
)

expect_invalid_numeric(
  structure_rejects_unknown_boolean
  "--protect-structure requires on or off"
  local-contrast --input missing.tiff --output output.tiff --protect-structure maybe
)
expect_invalid_numeric(
  structure_rejects_negative_radius
  "--fine-radius requires a positive integer"
  local-contrast --input missing.tiff --output output.tiff --fine-radius -1
)

expect_invalid_numeric(
  psf_rejects_nonfinite_covariance
  "--cov-xx requires a finite number"
  psf-match --input missing.fits --output output.fits --cov-xx nan --cov-yy 1 --cov-xy 0
)
expect_invalid_numeric(
  psf_rejects_display_output
  "psf-match requires linear FITS input and FITS output"
  psf-match --input missing.fits --output output.png --cov-xx 1 --cov-yy 1 --cov-xy 0
)

expect_invalid_numeric(
  nonfinite_float
  "--quality must be a number from 0 to 1"
  convert --input missing.tiff --output output.png --quality nan
)
expect_invalid_numeric(
  trailing_float_characters
  "--pixfrac must be a number greater than 0 and no greater than 1"
  drizzle --output output.tiff --pixfrac 1abc missing.tiff
)
expect_invalid_numeric(
  negative_unsigned_integer
  "--x must be a positive integer"
  crop --input missing.tiff --output output.png --x -1
)
expect_invalid_numeric(
  overflowing_unsigned_integer
  "--width must be a positive integer"
  preview --input missing.tiff --output output.png --width 4294967296
)
expect_invalid_numeric(
  crop_margin_above_maximum
  "--margin must be in [0, 0.45]"
  crop --input missing.tiff --output output.png --aspect 1:1 --margin 0.4501
)
expect_invalid_numeric(
  crop_margin_inclusive_maximum_reaches_input_validation
  "Input file does not exist"
  crop --input missing.tiff --output output.png --aspect 1:1 --margin 0.45
)
expect_invalid_numeric(
  convert_rejects_unknown_output_before_reading_input
  "Output image extension must be PNG, JPEG, HEIF, TIFF, or FITS"
  convert --input missing.tiff --output output.bmp
)
expect_invalid_numeric(
  convert_rejects_unknown_fits_value_mode
  "--fits-values must be display or scientific"
  convert --input missing.fits --output output.fits --fits-values raw
)
expect_invalid_numeric(
  convert_requires_fits_value_mode
  "--fits-values requires display or scientific"
  convert --input missing.fits --output output.fits --fits-values
)
expect_invalid_numeric(
  artifact_scientific_bridge_requires_fits_source
  "Scientific artifact removal requires a FITS input when the output is FITS"
  artifacts remove --input missing.png --output output.fits
)
expect_invalid_numeric(
  cloud_scientific_bridge_requires_fits_source
  "Scientific cloud removal requires a FITS input when the output is FITS"
  clouds remove --input missing.png --output output.fits
)
expect_invalid_numeric(
  background_rejects_invalid_preserve_brightness
  "--preserve-brightness must be on or off"
  background --input missing.fits --output output.fits --preserve-brightness maybe
)
expect_invalid_numeric(
  background_requires_preserve_brightness_value
  "--preserve-brightness requires on or off"
  background --input missing.fits --output output.fits --preserve-brightness
)
expect_invalid_numeric(
  background_rejects_invalid_protect_bright_targets
  "--protect-bright-targets must be on or off"
  background --input missing.fits --output output.fits --protect-bright-targets maybe
)
expect_invalid_numeric(
  background_requires_protect_bright_targets_value
  "--protect-bright-targets requires on or off"
  background --input missing.fits --output output.fits --protect-bright-targets
)
expect_invalid_numeric(
  raw_exposure_rejects_out_of_range_value
  "--raw-exposure-bias must be between -5 and 5 stops"
  convert --input missing.nef --output output.fits --raw-exposure-bias 5.01
)
expect_invalid_numeric(
  raw_temperature_rejects_out_of_range_value
  "--raw-temperature must be between 2000 and 50000 Kelvin"
  convert --input missing.nef --output output.fits --raw-temperature 1999
)
expect_invalid_numeric(
  raw_tint_rejects_nonfinite_value
  "--raw-tint must be a number"
  convert --input missing.nef --output output.fits --raw-tint nan
)
expect_invalid_numeric(
  raw_black_value_rejects_out_of_range_value
  "--raw-black-value must be between 0 and 1"
  convert --input missing.nef --output output.fits --raw-black-value 1.01
)
expect_invalid_numeric(
  calibrate_requires_linear_raw_decode
  "calibrate requires --raw-linear on"
  calibrate --light missing.nef --output output.fits --raw-linear off
)
expect_invalid_numeric(
  calibrate_requires_dark_bias_state
  "requires --dark-bias included|removed"
  calibrate --light missing.nef --dark dark.nef --bias bias.nef --output output.fits
)
expect_invalid_numeric(
  calibrate_requires_flat_bias_state
  "requires --flat-bias included|removed"
  calibrate --light missing.nef --flat flat.nef --bias bias.nef --output output.fits
)
expect_invalid_numeric(
  calibrate_rejects_invalid_bias_state
  "--dark-bias must be included or removed"
  calibrate --light missing.nef --dark dark.nef --output output.fits --dark-bias unknown
)
expect_invalid_numeric(
  calibrate_rejects_dark_bias_without_dark
  "--dark-bias requires --dark <image>"
  calibrate --light missing.nef --output output.fits --dark-bias included
)
expect_invalid_numeric(
  calibrate_rejects_flat_bias_without_flat
  "--flat-bias requires --flat <image>"
  calibrate --light missing.nef --output output.fits --flat-bias removed
)
expect_invalid_numeric(
  calibrate_rejects_included_flat_without_bias
  "--flat-bias included requires --bias <image>"
  calibrate --light missing.nef --flat flat.nef --output output.fits --flat-bias included
)
expect_invalid_numeric(
  master_requires_linear_raw_decode
  "master requires --raw-linear on"
  master --output output.fits --raw-linear off missing.nef
)

expect_invalid_numeric(
  develop_nonfinite_gain
  "--red-gain requires a finite number"
  develop --input missing.fits --output output.tiff --red-gain nan
)
expect_invalid_numeric(
  develop_nonfinite_exposure
  "--star-exposure requires a finite number"
  develop --input missing.fits --output output.tiff --star-exposure inf
)

expect_invalid_numeric(
  stack_invalid_interpolation
  "--interpolation requires bilinear or bicubic"
  stack --output output.fits --interpolation unknown missing.fits
)
expect_invalid_numeric(
  stack_invalid_fits_demosaic
  "--fits-demosaic requires bilinear, malvar, menon or ratio"
  stack --output output.fits --fits-demosaic unknown missing.fits
)
expect_invalid_numeric(
  convert_invalid_fits_demosaic
  "--fits-demosaic requires bilinear, malvar, menon or ratio"
  convert --input missing.fits --output output.fits --fits-demosaic unknown
)
expect_invalid_numeric(
  develop_invalid_tone_curve
  "--tone-curve requires rational or asinh"
  develop --input missing.fits --output output.tiff --tone-curve unknown
)
expect_invalid_numeric(
  develop_invalid_white_point
  "--white-point requires a finite number"
  develop --input missing.fits --output output.tiff --white-point nan
)
expect_invalid_numeric(
  background_invalid_exclusion
  "--exclude-ellipse requires five finite values and positive semiaxes"
  background --input missing.fits --output output.fits --model polynomial --exclude-ellipse 1,2,0,4,5
)
expect_invalid_numeric(
  stack_invalid_cfa_gains
  "--fits-cfa-gains requires three finite positive gains at most 20"
  stack --output output.fits --fits-cfa-gains 1,0,1 missing.fits
)
expect_invalid_numeric(
  stack_invalid_frame_weights
  "--frame-weights requires finite positive weights"
  stack --output output.fits --frame-weights 1,0,2 missing.fits
)
expect_invalid_numeric(
  stack_frame_weight_count
  "Frame weights must contain one finite positive value per input"
  stack --output output.fits --frame-weights 1,2 missing.fits
)

expect_invalid_numeric(nonlocal_nonfinite_h "--h has an invalid numeric value"
  denoise-nonlocal --h nan)
expect_invalid_numeric(nonlocal_negative_h "--h has an invalid numeric value"
  denoise-nonlocal --h -1)
expect_invalid_numeric(nonlocal_trailing_h "--h has an invalid numeric value"
  denoise-nonlocal --h 1tail)
expect_invalid_numeric(nonlocal_negative_weight "--red-weight has an invalid numeric value"
  denoise-nonlocal --red-weight -1)
expect_invalid_numeric(nonlocal_all_zero_weights "At least one luminance weight must be positive"
  denoise-nonlocal --red-weight 0 --green-weight 0 --blue-weight 0)

expect_invalid_numeric(continuum_curve_nonfinite "--continuum-curve requires input:output pairs"
  local-contrast --continuum-curve "0:0,nan:0.5,1:1")
expect_invalid_numeric(continuum_curve_fits_input "--continuum-curve requires display image input and output, not FITS"
  local-contrast --input missing.fits --output unused.tiff --continuum-curve "0:0,1:1")
expect_invalid_numeric(continuum_curve_fits_output "--continuum-curve requires display image input and output, not FITS"
  local-contrast --input missing.tiff --output unused.fits --continuum-curve "0:0,1:1")
