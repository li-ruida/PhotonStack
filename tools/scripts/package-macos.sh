#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
SWIFT_BUILD_DIR="$BUILD_DIR/swift"
APP_DIR="$BUILD_DIR/PhotonStack.app"
LEGACY_APP_DIR="$BUILD_DIR/PhotonStackMac.app"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
RESOURCES_DIR="$CONTENTS_DIR/Resources"
CLI_EXECUTABLE="$BUILD_DIR/release/apps/PhotonStackCLI/photonstack"
PREBUILT_SWIFT=""
if [[ "${1:-}" == "--prebuilt" && $# == 3 ]]; then
  PREBUILT_SWIFT="$2"
  CLI_EXECUTABLE="$3"
  [[ -x "$PREBUILT_SWIFT" && -x "$CLI_EXECUTABLE" ]] || {
    echo "Both prebuilt executables must exist and be executable." >&2
    exit 2
  }
elif [[ $# != 0 ]]; then
  echo "Usage: $0 [--prebuilt /path/PhotonStackMac /path/photonstack]" >&2
  exit 2
fi
ICON_SOURCE="$ROOT_DIR/apps/PhotonStackMac/Resources/AppIcon.png"
ICNS_PATH="$RESOURCES_DIR/AppIcon.icns"
PRODUCT_VERSION="$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")"
BUILD_NUMBER="$(date -u +%Y%m%d%H%M%S)"
BUILD_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
IMPORT_FILE_EXTENSIONS=(
  nef arw cr2 cr3 dng orf raf rw2 srw
  fits fit fts
  tif tiff png jpg jpeg heic heif
)
if [[ -n "${PHOTONSTACK_SOURCE_REVISION:-}" ]]; then
  GIT_COMMIT="${PHOTONSTACK_SOURCE_REVISION:0:12}"
elif [[ -n "${GITHUB_SHA:-}" ]]; then
  GIT_COMMIT="${GITHUB_SHA:0:12}"
elif GIT_COMMIT="$(git -C "$ROOT_DIR" rev-parse --short=12 HEAD 2>/dev/null)"; then
  if [[ -n "$(git -C "$ROOT_DIR" status --porcelain 2>/dev/null)" ]]; then
    GIT_COMMIT="${GIT_COMMIT}-dirty"
  fi
else
  GIT_COMMIT="unknown"
fi
BUILD_ID="v${PRODUCT_VERSION} (${GIT_COMMIT}, ${BUILD_NUMBER})"

if [[ -n "$PREBUILT_SWIFT" ]]; then
  EXECUTABLE="$PREBUILT_SWIFT"
else
"$ROOT_DIR/tools/scripts/build.sh" release

swift_build_args=(--package-path "$ROOT_DIR" --scratch-path "$SWIFT_BUILD_DIR" --configuration release --product PhotonStackMac)
if [[ -n "${SDKROOT:-}" ]]; then
  swift_build_args+=(--sdk "$SDKROOT")
fi
swift build "${swift_build_args[@]}"
# SwiftPM build systems can use different output layouts; package the binary
# from this invocation instead of a stale executable at a hard-coded path.
EXECUTABLE="$(swift build "${swift_build_args[@]}" --show-bin-path)/PhotonStackMac"
fi

rm -rf "$APP_DIR" "$LEGACY_APP_DIR"
mkdir -p "$MACOS_DIR" "$RESOURCES_DIR"
mkdir -p "$RESOURCES_DIR/ThirdPartyNotices"
cp "$ROOT_DIR/third_party/colour-demosaicing-LICENSE.txt" "$RESOURCES_DIR/ThirdPartyNotices/"
cp "$ROOT_DIR/third_party/pocketfft-LICENSE.md" "$RESOURCES_DIR/ThirdPartyNotices/"
cp "$ROOT_DIR/third_party/pocketfft-header-NOTICE.txt" "$RESOURCES_DIR/ThirdPartyNotices/"
cp "$EXECUTABLE" "$MACOS_DIR/PhotonStackMac"
cp "$CLI_EXECUTABLE" "$MACOS_DIR/photonstack"

if [[ -f "$ICON_SOURCE" ]] && command -v python3 >/dev/null 2>&1; then
  python3 - <<PY
from PIL import Image

source_path = r"$ICON_SOURCE"
output_path = r"$ICNS_PATH"
icon = Image.open(source_path).convert("RGBA")
icon.save(
    output_path,
    format="ICNS",
    sizes=[(16, 16), (32, 32), (128, 128), (256, 256), (512, 512), (1024, 1024)],
)
PY
fi

cat > "$CONTENTS_DIR/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>PhotonStackMac</string>
  <key>CFBundleIdentifier</key>
  <string>dev.photonstack.mac</string>
  <key>CFBundleIconFile</key>
  <string>AppIcon</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>PhotonStack</string>
  <key>CFBundleDisplayName</key>
  <string>PhotonStack</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>${PRODUCT_VERSION}</string>
  <key>CFBundleVersion</key>
  <string>${BUILD_NUMBER}</string>
  <key>PhotonStackBuildDate</key>
  <string>${BUILD_DATE}</string>
  <key>PhotonStackBuildIdentity</key>
  <string>${BUILD_ID}</string>
  <key>PhotonStackGitCommit</key>
  <string>${GIT_COMMIT}</string>
  <key>LSMinimumSystemVersion</key>
  <string>14.0</string>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key>
      <string>PhotonStack Supported Images</string>
      <key>CFBundleTypeRole</key>
      <string>Viewer</string>
      <key>LSHandlerRank</key>
      <string>Alternate</string>
      <key>CFBundleTypeExtensions</key>
      <array>
PLIST

for extension in "${IMPORT_FILE_EXTENSIONS[@]}"; do
cat >> "$CONTENTS_DIR/Info.plist" <<PLIST
        <string>${extension}</string>
PLIST
done

cat >> "$CONTENTS_DIR/Info.plist" <<PLIST
      </array>
    </dict>
  </array>
  <key>NSPrincipalClass</key>
  <string>NSApplication</string>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>NSQuitAlwaysKeepsWindows</key>
  <false/>
</dict>
</plist>
PLIST

cat > "$RESOURCES_DIR/build-info.json" <<JSON
{
  "version": "${PRODUCT_VERSION}",
  "build": "${BUILD_NUMBER}",
  "buildDate": "${BUILD_DATE}",
  "gitCommit": "${GIT_COMMIT}",
  "identity": "${BUILD_ID}"
}
JSON

echo "APPL????" > "$CONTENTS_DIR/PkgInfo"

if command -v codesign >/dev/null 2>&1; then
  codesign --force --sign - "$APP_DIR" >/dev/null
fi

echo "$APP_DIR"
