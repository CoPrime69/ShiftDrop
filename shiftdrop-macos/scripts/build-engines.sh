#!/usr/bin/env bash
# Builds everything the Mac app bundles that is not Swift: a static libwebp and an LGPL ffmpeg, both
# for Apple Silicon, into shiftdrop-macos/app/vendor (not tracked by git). Run it once, then
# shiftdrop-macos/app/build_app.sh. Needs: brew install cmake ninja meson pkgconf
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
"$here/build-libwebp-macos.sh"
"$here/build-ffmpeg-macos.sh"
