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
