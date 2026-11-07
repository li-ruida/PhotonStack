# Architecture

PhotonStack is a macOS-first astrophotography application with a shared C++
processing engine and a command-line interface.

## Repository layout

```text
apps/
  PhotonStackCLI/       C++ command-line executable
  PhotonStackMac/       SwiftUI macOS application entry point
engine/
  include/photonstack/  Public C++ engine headers
  src/                  C++20 engine implementation and Apple RAW decoder
packages/
  PhotonStackAppCore/   Project, settings, and workspace models
  PhotonStackProcessing/ Swift-to-engine processing services
  PhotonStackUI/        SwiftUI views and native macOS integrations
tests/                  C++ engine and CLI tests
tools/scripts/          Build, test, formatting, and packaging helpers
```

## Runtime structure

```text
SwiftUI workspace (PhotonStackUI)
        |
        +-- AppCore: project, recipes, settings and reports
        |
        +-- Processing services
                |                        |
          native engine bridge     bounded CLI process transport
                |                        |
                |                    C++ CLI
                +------------+-----------+
                             |
                       C++20 engine
                             |
          Apple imaging frameworks / FITS codec
```

The macOS app keeps project state, UI behavior, and file-panel integration in
Swift packages. Image-processing operations are implemented by the shared C++
engine and exposed to the app through the repository's bridge and CLI-facing
services. The CLI links the same engine directly.

## Workspace responsibilities

`PhotonStackRootView` composes the main split view and routes import, document,
workflow and navigation actions. Independent `Workspace*` views own the sidebar,
image preview, inspectors, products/layers, batch queue and console. Window-close
and application-state adapters live in `WorkspaceLifecycle.swift`; they preserve
the processing interruption and unsaved-document guards.

`PhotonStackWorkspaceModel` still owns workspace state and asynchronous task
generations. Selection changes, cancellation, undo and artifact publication must
stay coordinated here until narrower state owners can preserve those contracts.
Pure wire-report validation is extracted into `WorkspaceReportDecoder`, with
forwarding methods maintaining the existing model API. It has no dependency on
workspace selection, task ownership or view rendering.

`DeepSkySettingsView` reads labels, defaults, limits and applicability from the
engine schema. Its concise presentation hides advanced defaults, never modified
values; search covers every setting. It does not supply another set of numerical
defaults. `DeepSkyReview.evidenceCodes` is the shared source for attention counts,
frame filters and result-review cues. Missing measurements are not passing checks.

## Long-running processing boundaries

`ProcessOutputAccumulator` handles subprocess streams independently of the image
engine. It retains at most 32 MiB of complete structured stdout and 256 KiB of the
stderr tail. Progress records stream separately. A report overflow stops the
command with an explicit error; a truncated report must never become a successful
result. Diagnostic truncation is marked. Notifications are emitted outside the
accumulator lock, so observers can inspect or cancel their job without deadlock.
Output files are only published after the existing validation/atomic staging path
succeeds. User cancellation remains distinct from output overflow.

The engine's streaming average paths share `StreamingStackAccumulator`: Float64
signal and coverage accumulation, Float32 final image. This avoids Float32-sum
overflow and preserves small signed residuals between large finite values. Memory
is independent of frame count, but the accumulator costs twice as much as the old
Float32 one (about 127 MiB extra for 2160×3840 RGBA). This is a numerical precision
guarantee for tested cases, not photometric calibration or an image-quality score.

## Resource lifetime, result reuse and storage

Analysis-only runs measure eligible FITS frames concurrently within a CPU and
memory budget. Results are consumed in input order, including reference selection
and its tie break. Registration workers share a read-only reference image and
star catalog. Translation voting uses a bounded histogram and falls back to
the original vote ordering for tied peaks. On a second uncalibrated Malvar-guarded
FITS read, the decoder checks the scientific mosaic's SHA-256, image dimensions,
CFA phase, gains and mask setting before reapplying stellar corrections in order.
The bounded aperture data lives only for this analysis. Workers interpolate the
temporal sampling grid and the adjacent pixels used to measure noise with the
selected transform and interpolation method. With alignment disabled, the first decode supplies
both the native measurements and the temporal grid. The grids are released
after assessment. Completed reports retain the scalar measurements and decisions.
Sigma/MAD clipping reuses each worker's retained-sample scratch.

Robust stacking limits its tile data to `StackOptions.combineTileBytes` (64 MiB by
default). When a whole row across all frames exceeds this budget, tiles split rows
at pixel boundaries. A budget smaller than one all-frame pixel fails before any
input-consumed callback. File read buffers have a separate 64 MiB budget; output,
diagnostic, registration and finishing images remain additional allocations.

`DeepSkyPreflight` shares the canonical C++ recipe and header inspector. Opening
the resource estimate requests an asynchronous read-only plan. The view drops
stale results after settings or destination changes. Capacity reflects the disk
space available at the time of the check. See the CLI resource-planning contract
for the scope of the buffer and storage estimates.

`DeepSkyResultReuse` records an exact canonical recipe, app/CLI identities, source
content hashes, report hash and developed-image hash. Source identity includes
both the logical path and its resolved destination, so retargeted symbolic links
invalidate reuse even when the old destination remains unchanged. Verified full
resolution results can feed export replay directly; absent, legacy or mismatched
receipts fall back to the original replay. Sources are rechecked around processing
before publishing a reusable receipt. This avoids repeated calibration/registration
and stacking without substituting a preview for a full-resolution result.

Refinishing can retain an older scientific master across software builds. Its
source-content check is independent of the old renderer, but a new full-workflow
reuse receipt requires the original stack and current processing implementation to
match. A source snapshot verified before refinishing is checked again afterward.
Historical runs without content receipts retain their older size/mtime and
available master-digest checks; they do not acquire a stronger reuse guarantee.

Export destination validation protects project originals and processing
dependencies before replay and before conversion. Canonical paths, symlink targets
and filesystem identities cover path aliases and hard links. The image and its
provenance sidecar are both checked; ordinary unreferenced export files retain
their existing replacement behavior.

`WorkspaceStorageScanner` supplies both the read-only storage snapshot and cleanup
candidates. It retains unknown files and protects originals, graph/report/master
provenance, and undo/redo references; it does not traverse symbolic links. Cleanup
recomputes eligibility before deletion. The snapshot runs off the main actor and
only covers the displayed directory, not every project source or output volume.

## Verification boundaries

Keep numerical engine tests, CLI contract tests, Swift task/state tests, real FITS
acceptance and native UI inspection distinct. A successful report proves execution,
not the absence of residual gradients, clipping, trails or lost faint structure.
Real-image acceptance uses explicit input manifests, fresh output directories and
source hash checks; existing projects and originals are not fixtures.

## Build structure

The C++ engine and CLI use CMake presets. The macOS application is organized as
a Swift package. On Apple platforms, the engine builds its Objective-C++ RAW
decoder and links the Apple imaging frameworks declared in
[`engine/CMakeLists.txt`](../engine/CMakeLists.txt).

For build and packaging commands, see [`build.md`](build.md). For the current
user-facing capabilities, see [`features.md`](features.md).
