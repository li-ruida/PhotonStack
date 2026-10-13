#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP="$ROOT_DIR/build/PhotonStack.app"
OUTPUT="${1:-$ROOT_DIR/build/PhotonStack.dmg}"
[[ $# -le 1 && "$OUTPUT" == *.dmg ]] || {
  echo "Usage: $0 [/absolute/path/PhotonStack.dmg]" >&2
  exit 2
}
[[ -d "$APP" && ! -e "$OUTPUT" ]] || {
  echo "Package build/PhotonStack.app first; the output must not already exist." >&2
  exit 2
}
/usr/bin/codesign --verify --deep --strict "$APP"
STAGE=$(mktemp -d "$ROOT_DIR/build/dmg-stage.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
/usr/bin/ditto "$APP" "$STAGE/PhotonStack.app"
/bin/ln -s /Applications "$STAGE/Applications"
/usr/bin/hdiutil create -volname PhotonStack -srcfolder "$STAGE" \
  -fs HFS+ -format UDZO "$OUTPUT"
/usr/bin/hdiutil verify "$OUTPUT"
echo "Created $OUTPUT"
