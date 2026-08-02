#!/usr/bin/env bash
set -euo pipefail

mode="${1:-run}"
app_name="PhotonStackMac"
bundle_id="dev.photonstack.app"

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
app_bundle="$root_dir/build/PhotonStack.app"
app_binary="$app_bundle/Contents/MacOS/$app_name"

usage() {
  cat <<'EOF'
Usage: tools/scripts/run-macos-dev.sh [run|debug|logs|telemetry|verify]

Modes:
  run        Build, package, and launch the macOS app
  debug      Build, package, and launch the app binary in lldb
  logs       Build, package, launch, and stream app logs
  telemetry  Build, package, launch, and stream curves telemetry logs
  verify     Build, package, launch, and confirm the app process starts
EOF
}

build_app() {
  "$root_dir/tools/scripts/package-macos.sh" >/dev/null
}

open_app() {
  /usr/bin/open -n "$app_bundle"
}

stream_logs() {
  local predicate="$1"
  /usr/bin/log stream --info --style compact --predicate "$predicate"
}

pkill -x "$app_name" >/dev/null 2>&1 || true

build_app

case "$mode" in
  run)
    open_app
    ;;
  debug)
    lldb -- "$app_binary"
    ;;
  logs)
    open_app
    stream_logs "process == \"$app_name\""
    ;;
  telemetry)
    open_app
    stream_logs "subsystem == \"$bundle_id\" AND category == \"curves\""
    ;;
  verify)
    open_app
    sleep 1
    pgrep -x "$app_name" >/dev/null
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
