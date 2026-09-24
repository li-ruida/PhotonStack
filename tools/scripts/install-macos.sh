#!/usr/bin/env bash
set -euo pipefail

# Upgrade the same installed application; do not launch a second review identity.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOURCE="$ROOT_DIR/build/PhotonStack.app"
DESTINATION="/Applications/PhotonStack.app"
BACKUP_DIR="$ROOT_DIR/artifacts/app-backups"
BUNDLE_ID="dev.photonstack.mac"
[[ $# == 0 ]] || { echo "Usage: $0 (install the existing build/PhotonStack.app)" >&2; exit 2; }
for bundle in "$SOURCE" "$DESTINATION"; do
  if [[ -e "$bundle" ]]; then
    actual_id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$bundle/Contents/Info.plist")
    [[ "$actual_id" == "$BUNDLE_ID" ]] || { echo "Unexpected application identity: $bundle" >&2; exit 2; }
  fi
done
/usr/bin/codesign --verify --deep --strict "$SOURCE"
mkdir -p "$BACKUP_DIR"
stage=$(mktemp -d /Applications/.PhotonStack-update.XXXXXX)
restore_on_exit() {
  if [[ -d "$stage/previous.app" && ! -e "$DESTINATION" ]]; then
    mv "$stage/previous.app" "$DESTINATION"
  fi
  # Keep a previous bundle if rollback could not restore it.
  if [[ ! -d "$stage/previous.app" ]]; then rm -rf "$stage"; fi
}
trap restore_on_exit EXIT
/usr/bin/ditto "$SOURCE" "$stage/PhotonStack.app"
/usr/bin/codesign --verify --deep --strict "$stage/PhotonStack.app"
if [[ -e "$DESTINATION" ]]; then
  backup="$BACKUP_DIR/PhotonStack-$(date -u +%Y%m%dT%H%M%SZ)-$$.zip"
  /usr/bin/ditto -c -k --sequesterRsrc --keepParent "$DESTINATION" "$backup"
  /usr/bin/unzip -tq "$backup" >/dev/null
  mv "$DESTINATION" "$stage/previous.app"
  echo "Previous version: $backup"
fi
mv "$stage/PhotonStack.app" "$DESTINATION"
rm -rf "$stage/previous.app"
echo "Updated $DESTINATION ($BUNDLE_ID). Relaunch this same application to use the update."
