#!/usr/bin/env bash
set -euo pipefail

# Deliberately separate from run-macos-dev.sh: never kill or replace a user App.
qa_mode="${1:-run}"
[[ "$qa_mode" == run || "$qa_mode" == --stage-only ]] || {
  echo 'Usage: tools/scripts/run-macos-quality-qa.sh [run|--stage-only]' >&2
  exit 2
}
qa_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if pgrep -f '/PhotonStackQualityQA.app/Contents/MacOS/PhotonStackQualityQA$' >/dev/null; then
  echo 'A QA instance is already running; inspect or close that instance first.' >&2
  exit 1
fi
qa_cli="$qa_root/build/release/apps/PhotonStackCLI/photonstack"
[[ -x "$qa_cli" ]] || { echo 'Build the CLI first.' >&2; exit 1; }
swift build --package-path "$qa_root" --product PhotonStackQualityQA
qa_bin="$(swift build --package-path "$qa_root" --show-bin-path)/PhotonStackQualityQA"
mkdir -p "$qa_root/artifacts"
qa_run="$(mktemp -d "$qa_root/artifacts/quality-qa-XXXXXXXX")"
qa_bundle="$qa_run/PhotonStackQualityQA.app"
mkdir -p "$qa_bundle/Contents/MacOS" "$qa_run/data"
cp "$qa_bin" "$qa_bundle/Contents/MacOS/PhotonStackQualityQA"
cp "$qa_cli" "$qa_bundle/Contents/MacOS/photonstack"
cp "$qa_root/tools/PhotonStackQualityQA/Info.plist" "$qa_bundle/Contents/Info.plist"
plutil -insert PhotonStackQADataRoot -string "$qa_run/data" "$qa_bundle/Contents/Info.plist"
codesign --force --sign - "$qa_bundle"
echo "$qa_bundle"
if [[ "$qa_mode" == run ]]; then /usr/bin/open -n "$qa_bundle"; fi
