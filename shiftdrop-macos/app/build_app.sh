#!/bin/zsh
# Builds ShiftDrop.app (a menu-bar app) so macOS can attach the Input Monitoring permission to it.
set -euo pipefail
cd "$(dirname "$0")"
# The engines (a static libwebp and ShiftDrop's own LGPL ffmpeg) are built once by scripts/build-engines.sh.
if [ ! -f vendor/libwebp/lib/libwebp.a ] || [ ! -x vendor/ffmpeg/ffmpeg ]; then
  echo "vendor/libwebp or vendor/ffmpeg is missing. Build them once with:" >&2
  echo "  brew install cmake ninja meson pkgconf && ../scripts/build-engines.sh" >&2
  exit 1
fi
swift build -c release 2>&1 | grep -v "ld: warning" || true
APP=build/ShiftDrop.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp .build/release/ShiftDrop "$APP/Contents/MacOS/"
# ffmpeg runs as a child process (FfmpegEngine.locate), and its licence and source travel with it (LGPL).
mkdir -p "$APP/Contents/Resources/engines" "$APP/Contents/Resources/licenses"
cp vendor/ffmpeg/ffmpeg "$APP/Contents/Resources/engines/"
cp vendor/ffmpeg/FFMPEG-LICENSE.txt vendor/ffmpeg/FFMPEG-SOURCE.txt vendor/ffmpeg/ffmpeg-sources.tar.xz "$APP/Contents/Resources/licenses/"
mkdir -p "$APP/Contents/Resources/recipes" && cp ../recipes/builtin.json "$APP/Contents/Resources/recipes/"
cp vendor/libwebp/LICENSE-libwebp.txt vendor/libwebp/SOURCE-libwebp.txt "$APP/Contents/Resources/licenses/"
cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>dev.shiftdrop.mac</string>
  <key>CFBundleName</key><string>ShiftDrop</string>
  <key>CFBundleExecutable</key><string>ShiftDrop</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>0.0.1</string>
  <key>LSMinimumSystemVersion</key><string>13.4</string>
  <key>LSUIElement</key><true/>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
# Optional: sign with a stable local identity so the Input Monitoring permission survives rebuilds.
# Set SHIFTDROP_SIGN_KEYCHAIN (a keychain file) and SHIFTDROP_SIGN_KEYCHAIN_PASSWORD; otherwise the app is ad-hoc signed.
KC="${SHIFTDROP_SIGN_KEYCHAIN:-}"
if [ -n "$KC" ] && [ -f "$KC" ] && security unlock-keychain -p "${SHIFTDROP_SIGN_KEYCHAIN_PASSWORD:-}" "$KC" 2>/dev/null; then
  # Stable identity: macOS keeps the Input Monitoring permission across rebuilds.
  codesign --force --sign "ShiftDrop Dev" --keychain "$KC" "$APP/Contents/Resources/engines/ffmpeg"
  codesign --force --sign "ShiftDrop Dev" --keychain "$KC" "$APP"
else
  echo "no ShiftDrop Dev identity: ad-hoc signing (permissions reset on every build)"
  codesign --force --sign - "$APP/Contents/Resources/engines/ffmpeg"
  codesign --force --sign - "$APP"
fi
echo "built $APP  (run: open $APP)"
