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

## See Also

- [`README.md`](../README.md)
- [`docs/features.md`](features.md)
- [`docs/image-pipeline.md`](image-pipeline.md)
