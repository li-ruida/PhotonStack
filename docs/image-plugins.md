# Local image processing extensions

The **Post-processing → Advanced → Processing extensions** panel loads a versioned local executable definition, shows its name, path and arguments, and runs it only when **Run & save** is selected. Loading a definition does not execute the program. The same workflow is available from the CLI:

```sh
photonstack plugin-inspect --definition tool.psplugin
photonstack plugin-run --definition tool.psplugin --input linear.fits --output-dir new-result
```

## Version 1 contract

A definition is UTF-8, with one record per line and C++ quoted-string escaping (`\"` and `\\`). Every singleton field below is required; `argument` is repeatable and ordered. Paths are absolute. Each placeholder must occupy one entire argument. Arguments are passed directly to the executable, without a shell.

```text
PhotonStack-ImagePlugin-1
id "org.example.linear-tool"
name "Example linear tool"
executable "/absolute/path/to/tool"
sha256 "REPLACE_WITH_SHA256_OF_EXECUTABLE"
domain "linear-fits"
grid "preserve"
units "preserve"
timeout "120"
argument "--input"
argument "{input}"
argument "--output"
argument "{output}"
```

The executable hash binds the installed binary, not all its imported modules, model weights or operating-system libraries. Authors should distribute and version those dependencies separately. Definitions containing interpreter scripts are subject to the same limitation. A definition is not a sandbox or a trust signature: run trusted local software only.

Version 1 accepts mono/RGB/RGBA linear FITS, with no CFA mosaic and no nonlinear display encoding. The exact original FITS is copied to an isolated job directory and supplied as input. No neural normalization, clipping, debayering or display stretch is implied. Programs write a new FITS output with the same sample-unit label, dimensions, channel count, coverage and WCS. Source and staged input hashes must remain unchanged.

Outputs are read through the native linear FITS validator. Coverage is compared pixel by pixel. If a WCS is present, both images must retain it and an 81-point grid must agree within 0.0001 pixel. The grid and physical units remain an author declaration: metadata and dimensions cannot detect every image shift or prove scientific correctness. A plugin that destroys genuine faint detail can still satisfy this structural contract. Scientific quality needs separate validation.

## Execution and outputs

The application pins the inspected definition hash when running, preventing a changed definition from being executed without another review. The CLI optionally accepts `--definition-sha256` for the same behavior. The executable hash is checked before and after execution.

Each job has a 1–600 second timeout, a bounded log and process-group cancellation. Success also terminates remaining helpers. Nonzero exits, missing/invalid outputs, new invalid pixels, changed coverage, symlink outputs, changed inputs, lost WCS or changed unit labels fail before publication. Existing output directories cannot be overwritten.

The new result folder contains:

- `input.fits`: original input bytes.
- `result.fits`: validated external output bytes.
- `definition.psplugin`: exact definition used.
- `plugin.log`: combined process output.
- `plugin.json`: source, definition, executable and result hashes, execution time and declared contract.

A result is imported as a derived image in the App. Source files and the installed application are not replaced.

If a tool requires explicit acceptance of its own license, review the complete terms before adding `license-file "/absolute/path/LICENSE"` and `accepted-license-sha256 "..."` records. The hash is checked at execution; changed terms require another review. The integration does not grant rights to third-party software or bundle it. In particular, no StarNet model or runtime is distributed here and no license is accepted automatically.

## Validation

`test_image_plugin.py` runs an actual external Python gain fixture and independently checks signed pixel values and provenance. It also covers invalid data, input mutation, coverage changes, unit-label changes, symlink output, timeout, errors and atomic cleanup. The gain fixture proves the process contract, not the quality of a third-party astronomical algorithm. The Swift integration test loads a definition, runs through the real CLI, and rejects changed definitions and existing destinations.
