#!/usr/bin/env bash
# Builds libwebp as a static library for Apple Silicon, so the Mac app links it from
# shiftdrop-macos/app/vendor/libwebp instead of from Homebrew. libwebp is BSD-licensed.
#
#   brew install cmake ninja            (once)
#   shiftdrop-macos/scripts/build-libwebp-macos.sh [work dir] [output dir]
#   (defaults: shiftdrop-macos/build/engines-work, shiftdrop-macos/app/vendor/libwebp)
#
# The source is pinned: a git tag and the commit it must resolve to. A moved tag stops the build.
# Works with the bash 3.2 that macOS ships.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
work=$(mkdir -p "${1:-$root/build/engines-work}" && cd "${1:-$root/build/engines-work}" && pwd)
out=$(mkdir -p "${2:-$root/app/vendor/libwebp}" && cd "${2:-$root/app/vendor/libwebp}" && pwd)

# Keep in step with MIN_MACOS in build-ffmpeg-macos.sh and platforms in app/Package.swift.
MIN_MACOS=13.4
NAME=libwebp URL=https://github.com/webmproject/libwebp TAG=v1.6.0 COMMIT=4fa21912338357f89e4fd51cf2368325b59e9bd9

for tool in git cmake ninja; do
  command -v "$tool" >/dev/null || { echo "$tool is missing. Install the build tools: brew install cmake ninja" >&2; exit 1; }
done

src=$work/src/$NAME
mkdir -p "$work/src"
if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null || true)" != "$COMMIT" ]; then
  rm -rf "$src"
  echo "=== fetch $NAME $TAG"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" "$URL" "$src"
fi
got=$(git -C "$src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "$NAME: tag $TAG is $got, expected $COMMIT" >&2; exit 1; }

echo "=== build $NAME $TAG (arm64, macOS $MIN_MACOS+, static)"
rm -rf "$work/libwebp-build" "$out"
mkdir -p "$out"
cmake -S "$src" -B "$work/libwebp-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$out" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF \
  -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF \
  -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF >/dev/null
cmake --build "$work/libwebp-build" -j "$(sysctl -n hw.ncpu)"
cmake --install "$work/libwebp-build" >/dev/null

cp "$src/COPYING" "$out/LICENSE-libwebp.txt"
cat >"$out/SOURCE-libwebp.txt" <<EOF
libwebp $TAG, built from unmodified source: $URL (commit $COMMIT).
Licence: BSD 3-clause (LICENSE-libwebp.txt). Built by shiftdrop-macos/scripts/build-libwebp-macos.sh.
EOF

for f in lib/libwebp.a lib/libsharpyuv.a include/webp/encode.h; do
  [ -f "$out/$f" ] || { echo "missing $out/$f after the build" >&2; exit 1; }
done
echo "libwebp is in $out"
