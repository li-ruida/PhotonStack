# XISF exchange

The Import menu offers **XISF exchange** in both directions. An import converts one selected image to FITS and adds it to the current project. Export converts the FITS primary image to XISF. Each operation creates a new directory containing the image, `metadata.xml` and a SHA-256 provenance record, without changing the source.

```sh
photonstack xisf-import --input master.xisf --image 0 --output-dir imported
photonstack xisf-export --input master.fits --compression zlib --output-dir exported
```

## Pixel preservation

- Monolithic XISF 1.0, attached two-dimensional Gray/RGB image blocks, mono/RGB/RGBA, planar or interleaved storage, little or big endian.
- UInt8, UInt16, UInt32, Float32 and Float64 are repacked directly, without a Float32 intermediate. Signed float values, NaNs, infinities and negative zero remain intact. Unsigned FITS uses the standard signed-storage `BZERO` convention.
- Import supports uncompressed and zlib data; LZ4/LZ4HC and Zstandard are enabled when their static development libraries are available at build time. Byte shuffling and compression subblocks are supported. The macOS local validation build includes both optional codecs.
- SHA-1/256/512 pixel-block checksums, when present, are checked against the stored compressed bytes before decompression. Export writes SHA-256 and either zlib or uncompressed blocks.
- A 2×2 RGB Bayer `ColorFilterArray` maps to FITS Bayer metadata. FITS keyword WCS, acquisition metadata and units are retained. No debayering, interpolation, exposure scaling, gamma, display function or ICC color conversion is performed.
- The exchange preserves sample values. It does not establish that those values are calibrated or linear. PhotonStack's downstream processing remains Float32; a lossless Float64/UInt32 interchange does not change that processing precision.

## Explicit limits

Each decoded image is limited to 512 MiB and the XML header to 4 MiB. One image is selected by zero-based index. Distributed files, inline/embedded pixel blocks, complex samples, UInt64, non-RGB color spaces, non-Bayer CFA, rotated/flipped orientation and bottom-up row declarations are rejected, rather than silently interpreted. DTDs and external entities are disabled.

The full XML is retained as an artifact, but arbitrary XISF `Property` metadata, PixInsight astrometric solution properties, previews, display functions and color profiles are not translated to FITS. Property-only astrometry/acquisition information therefore needs explicit conversion or a new solve/declaration before downstream use. XML is an archive of the original metadata, not a promise that FITS has equivalent metadata for every namespace.

FITS export accepts unscaled Float32/64, UInt8 and the standard unsigned UInt16/32 convention. Signed integers, other BSCALE/BZERO mappings and integer BLANK require explicit conversion first. Nonzero Bayer offsets and bottom-up orientation also require explicit conversion. FITS extension HDUs remain in the source and are not exported. Metadata that cannot fit a standard ASCII FITS card is rejected on import. The source is never deleted.

Floating-point export uses nominal bounds `0:1` while retaining all stored values, including values outside that interval. This is not an automatic display or ADU normalization. Other applications may choose their own visualization of such data.

## Build and verification

CMake requires system libxml2 and zlib. Optional LZ4 and Zstandard decoders link static libraries, avoiding a runtime dependency on Homebrew in packaged macOS apps; their BSD license notices accompany the app. Install their development packages before configuring to enable them. Missing optional decoders produce an explicit error for affected files.

`test_xisf_exchange.py` constructs independent byte fixtures across types, layouts, endian orders and compressed blocks, verifies exact FITS sample bytes after round trips, and rejects invalid checksums, blocks, XML and unsupported orientation. `XisfExchangeTests.swift` exercises the actual CLI through the workspace import service and verifies product/source/metadata hashes.

Format references: [XISF](https://pixinsight.com/xisf/), [N.I.N.A. metadata](https://www.nighttime-imaging.eu/docs/master/site/advanced/file_formats/xisf/), [independent XISF data model](https://docs.rs/xisf/latest/xisf/).
