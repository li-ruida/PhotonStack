#!/usr/bin/env bash
set -euo pipefail

mode="${1:-run}"
case "$mode" in
  --debug) mode="debug" ;;
  --logs) mode="logs" ;;
  --telemetry) mode="telemetry" ;;
  --verify) mode="verify" ;;
esac

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec /usr/bin/env bash "$root_dir/tools/scripts/run-macos-dev.sh" "$mode"
