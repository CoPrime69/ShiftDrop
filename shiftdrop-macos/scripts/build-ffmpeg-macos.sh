#!/usr/bin/env bash
# Builds ShiftDrop's own ffmpeg for Apple Silicon: FFmpeg and the few libraries it needs, linked
# statically into one executable that only depends on macOS system libraries and frameworks. LGPL only:
# no --enable-gpl, no --enable-nonfree (so no x264 or x265; H.264 and HEVC use VideoToolbox, the
# Mac's own hardware encoder).
#
# It follows scripts/build-ffmpeg.sh (the Windows build) and uses the same pinned sources and the
# same component list (scripts/ffmpeg-components.txt), minus the Windows-only GPU encoders.
#
#   brew install cmake ninja meson pkgconf      (once)
#   shiftdrop-macos/scripts/build-ffmpeg-macos.sh [work dir] [output dir]
#   (defaults: shiftdrop-macos/build/engines-work, shiftdrop-macos/app/vendor/ffmpeg)
#
# Every source is pinned: a git tag and the commit it must resolve to, or a tarball and its SHA-256.
# A moved tag or a changed download stops the build. The output folder gets ffmpeg,
# FFMPEG-LICENSE.txt (FFmpeg's and every library's licence), FFMPEG-SOURCE.txt (versions, sources and
# the exact configure line) and ffmpeg-sources.tar.xz (the complete corresponding source).
#
# Written for the bash 3.2 that macOS ships (no mapfile, no associative arrays).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
repo=$(cd "$root/.." && pwd)
work=$(mkdir -p "${1:-$root/build/engines-work}" && cd "${1:-$root/build/engines-work}" && pwd)
out=$(mkdir -p "${2:-$root/app/vendor/ffmpeg}" && cd "${2:-$root/app/vendor/ffmpeg}" && pwd)
shared_components=$repo/scripts/ffmpeg-components.txt
components=$work/components-macos.txt
src=$work/src
prefix=$work/prefix
jobs=$(sysctl -n hw.ncpu)

# Keep in step with MIN_MACOS in build-libwebp-macos.sh and platforms in app/Package.swift.
MIN_MACOS=13.4

# name | git repository | tag | commit the tag must point at | licence file(s) in the tree
sources=(
  "ffmpeg|https://github.com/FFmpeg/FFmpeg|n8.1.3|1041abdc962f4cc4f394aa8de9dc5236c0c3b9e7|LICENSE.md COPYING.LGPLv2.1"
  "dav1d|https://github.com/videolan/dav1d|1.5.4|54706fc6bc0cdecab7e9593974a4039cc038fca7|COPYING"
  "libvpx|https://github.com/webmproject/libvpx|v1.17.0|6df3ec34557879fff673706f4a1d9fbd0f3a6f0e|LICENSE PATENTS"
  "opus|https://github.com/xiph/opus|v1.6.1|22244de5a79bd1d6d623c32e72bf1954b56235be|COPYING"
  "ogg|https://github.com/xiph/ogg|v1.3.6|be05b13e98b048f0b5a0f5fa8ce514d56db5f822|COPYING"
  "vorbis|https://github.com/xiph/vorbis|v1.3.7|0657aee69dec8508a0011f47f3b69d7538e9d262|COPYING"
)
# name | tarball | SHA-256 | licence file(s)
tarballs=(
  "lame|https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz|ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e|COPYING LICENSE"
)

log() { printf '\n=== %s\n' "$*"; }
field() { local IFS='|'; set -f; local a=($1); set +f; echo "${a[$2]}"; }

for tool in git curl xz make clang cmake ninja meson pkg-config perl shasum; do
  command -v "$tool" >/dev/null || {
    echo "$tool is missing. Install the build tools: brew install cmake ninja meson pkgconf" >&2; exit 1; }
done

fetch() {
  mkdir -p "$src"
  local s t
  for s in "${sources[@]}"; do
    local name url tag commit got
    name=$(field "$s" 0) url=$(field "$s" 1) tag=$(field "$s" 2) commit=$(field "$s" 3)
    if [ "$(git -C "$src/$name" rev-parse HEAD 2>/dev/null || true)" != "$commit" ]; then
      rm -rf "$src/$name"
      log "fetch $name $tag"
      git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$url" "$src/$name"
    fi
    got=$(git -C "$src/$name" rev-parse HEAD)
    [ "$got" = "$commit" ] || { echo "$name: tag $tag is $got, expected $commit" >&2; exit 1; }
  done
  for t in "${tarballs[@]}"; do
    local name url sha file
    name=$(field "$t" 0) url=$(field "$t" 1) sha=$(field "$t" 2)
    file=$src/$(basename "$url")
    if [ ! -f "$file" ] || [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" != "$sha" ]; then
      log "fetch $name"
      curl -fsSL --retry 5 --retry-all-errors -o "$file" "$url"
    fi
    [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" = "$sha" ] || { echo "$name: SHA-256 mismatch" >&2; rm -f "$file"; exit 1; }
    if [ ! -d "$src/$name" ]; then
      mkdir -p "$src/$name"
      tar -xzf "$file" -C "$src/$name" --strip-components=1
    fi
  done
}

# ---- toolchain -----------------------------------------------------------------------------------

export MACOSX_DEPLOYMENT_TARGET=$MIN_MACOS
export CC=clang CXX=clang++
# Only our own prefix: never Homebrew's libraries (they may be GPL or newer than the deployment target).
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig PKG_CONFIG_PATH=
export CFLAGS="-arch arm64 -mmacosx-version-min=$MIN_MACOS -ffunction-sections -fdata-sections"
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="-I$prefix/include"
export LDFLAGS="-arch arm64 -mmacosx-version-min=$MIN_MACOS -L$prefix/lib"

cmake_build() {   # <source dir> <cmake options...>
  local dir=$1; shift
  rm -rf "$dir/_build"
  cmake -S "$dir" -B "$dir/_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@" >/dev/null
  cmake --build "$dir/_build" -j "$jobs"
  cmake --install "$dir/_build" >/dev/null
}

# ---- libraries -------------------------------------------------------------------------------------

build_deps() {
  rm -rf "$prefix"
  mkdir -p "$prefix/include" "$prefix/lib/pkgconfig"

  log dav1d
  rm -rf "$src/dav1d/_build"
  meson setup "$src/dav1d/_build" "$src/dav1d" --prefix "$prefix" --libdir lib --buildtype release \
    --default-library static -Denable_tools=false -Denable_tests=false -Denable_examples=false >/dev/null
  ninja -C "$src/dav1d/_build" install >/dev/null

  log libvpx
  # VP9 encoding only (FFmpeg's own decoders are faster for VP8/VP9); high bit depth as in the
  # Windows build, so 10-bit sources keep their depth in WebM. The newest arm64-darwin target this
  # libvpx knows; --extra-cflags then sets the deployment target the target name would raise.
  local vpx_target
  vpx_target=$( ("$src/libvpx/configure" --help 2>&1 || true) | grep -o 'arm64-darwin[0-9][0-9]*-gcc' | sort -uV | tail -1)
  [ -n "$vpx_target" ] || { echo "libvpx has no arm64-darwin target" >&2; exit 1; }
  (cd "$src/libvpx" && rm -rf _build && mkdir _build && cd _build && \
    ../configure --target="$vpx_target" --prefix="$prefix" --extra-cflags="-mmacosx-version-min=$MIN_MACOS" \
      --disable-shared --enable-static --enable-pic --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-vp8 --disable-vp9-decoder --enable-vp9-highbitdepth --enable-runtime-cpu-detect >/dev/null && \
    make -j "$jobs" >/dev/null && make install >/dev/null)

  log opus
  cmake_build "$src/opus" -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF

  log ogg
  cmake_build "$src/ogg" -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
  log vorbis
  cmake_build "$src/vorbis" -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_TESTING=OFF

  log lame
  # lame 3.100 is from 2017: its config.guess does not know Apple Silicon, so name the platform.
  (cd "$src/lame" && (make distclean >/dev/null 2>&1 || true) && \
    ./configure --build=arm-apple-darwin --host=arm-apple-darwin --prefix="$prefix" --disable-shared --enable-static \
      --disable-frontend --disable-decoder --disable-gtktest >/dev/null && make -j "$jobs" >/dev/null && make install >/dev/null)
}

# ---- FFmpeg ------------------------------------------------------------------------------------------

# The shared component list (scripts/ffmpeg-components.txt) is the Windows build's. Take out its
# Windows-only GPU/Media Foundation/OpenH264 encoders and Direct3D/NVDEC decoders, and put in the
# Mac's VideoToolbox ones.
make_components() {
  awk '
    /^\[/ {
      section = $0; print
      if (section == "[encoders]") {
        print "# macOS: the hardware H.264 and HEVC encoders (Media.swift, h264)."
        print "h264_videotoolbox hevc_videotoolbox"
      }
      next
    }
    section == "[encoders]" && /^[A-Za-z0-9*_]/ {
      n = split($0, words, " "); line = ""
      for (i = 1; i <= n; i++) {
        w = words[i]
        if (w == "h264_nvenc" || w == "h264_qsv" || w == "h264_amf" || w == "h264_mf" || w == "libopenh264") continue
        line = line (line == "" ? "" : " ") w
      }
      if (line != "") print line
      next
    }
    section == "[hwaccels]" && /^[A-Za-z0-9*_]/ { print "*_videotoolbox"; next }
    { print }
  ' "$shared_components" >"$components"
}

# The configure name of a component, from the name ffmpeg prints (scripts/ffmpeg-components.txt).
configure_name() {
  case "$1:$2" in
    muxers:3gp) echo tgp ;;
    muxers:mpeg) echo mpeg1system ;;
    muxers:s16le) echo pcm_s16le ;;
    demuxers:mpeg) echo mpegps ;;
    demuxers:shn) echo shorten ;;
    decoders:012v) echo zero12v ;;
    decoders:8bps) echo eightbps ;;
    decoders:msmpeg4) echo msmpeg4v3 ;;
    decoders:camtasia) echo tscc ;;
    decoders:atrac3plus) echo atrac3p ;;
    decoders:atrac3plusal) echo atrac3pal ;;
    decoders:real_144) echo ra_144 ;;
    decoders:real_288) echo ra_288 ;;
    *) echo "${2//-/_}" ;;
  esac
}

kind_of() {
  case "$1" in
    encoders) echo encoder ;; decoders) echo decoder ;; muxers) echo muxer ;; demuxers) echo demuxer ;;
    filters) echo filter ;; parsers) echo parser ;; bsfs) echo bsf ;; protocols) echo protocol ;;
    indevs) echo indev ;; hwaccels) echo hwaccel ;; *) echo "" ;;
  esac
}

# --enable-<type>=a,b,c for each section of the components file. Patterns are expanded against
# configure's own lists, and a name configure doesn't know stops the build.
component_flags() {
  local section="" line name cname kind list matched re
  set -f
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line%%#*}
    [[ $line =~ ^[[:space:]]*$ ]] && continue
    if [[ $line =~ ^\[([a-z]+)\] ]]; then
      section=${BASH_REMATCH[1]}
      kind=$(kind_of "$section")
      [ -n "$kind" ] || { echo "unknown section [$section] in $components" >&2; exit 1; }
      list=$work/list-$section.txt
      [ -s "$list" ] || "$src/ffmpeg/configure" --list-$section | tr -s ' \t' '\n' | sort -u >"$list"
      continue
    fi
    kind=$(kind_of "$section")
    list=$work/list-$section.txt
    for name in $line; do
      cname=$(configure_name "$section" "$name")
      if [[ $cname == *'*'* ]]; then
        re="^${cname//\*/.*}\$"
        matched=$(grep -E "$re" "$list" | tr '\n' ',' | sed 's/,$//')
        [ -n "$matched" ] || { echo "$section: $name matches nothing" >&2; exit 1; }
        echo "--enable-$kind=$matched"
      else
        grep -qx "$cname" "$list" || { echo "$section: configure has no $cname ($name)" >&2; exit 1; }
        echo "--enable-$kind=$cname"
      fi
    done
  done <"$components"
  set +f
}

ffmpeg_flags() {
  cat <<EOF
--prefix=$prefix
--arch=arm64
--target-os=darwin
--cc=clang
--pkg-config=pkg-config
--pkg-config-flags=--static
--extra-cflags=-I$prefix/include
--extra-cflags=-mmacosx-version-min=$MIN_MACOS
--extra-ldflags=-L$prefix/lib
--extra-ldflags=-mmacosx-version-min=$MIN_MACOS
--extra-ldflags=-Wl,-dead_strip
--enable-static
--disable-shared
--enable-pic
--disable-debug
--disable-doc
--disable-ffplay
--disable-ffprobe
--disable-network
--disable-autodetect
--disable-everything
--enable-pthreads
--enable-zlib
--enable-libdav1d
--enable-libvpx
--enable-libopus
--enable-libvorbis
--enable-libmp3lame
--enable-videotoolbox
EOF
  component_flags
}

build_ffmpeg() {
  log ffmpeg
  local b=$src/ffmpeg/_build line
  rm -rf "$b" && mkdir -p "$b"
  make_components
  ffmpeg_flags >"$work/configure-flags.txt"
  local flags=()
  while IFS= read -r line; do flags+=("$line"); done <"$work/configure-flags.txt"
  (cd "$b" && ../configure "${flags[@]}") || { tail -50 "$b/ffbuild/config.log"; exit 1; }
  make -C "$b" -j "$jobs" ffmpeg
  # Stripping drops the signature that arm64 macOS needs, so sign it again (ad hoc; the app build
  # signs it for real).
  strip -x -o "$out/ffmpeg" "$b/ffmpeg"
  codesign --force --sign - "$out/ffmpeg"
  # No GPL or non-free code may be in it.
  grep -q '^#define CONFIG_GPL 0' "$b/config.h" && grep -q '^#define CONFIG_NONFREE 0' "$b/config.h" || {
    echo "the build is not LGPL-only" >&2; exit 1; }
}

# ---- checks, notices and the corresponding source ----------------------------------------------------

check_binary() {
  log "check ffmpeg"
  local libs
  libs=$(otool -L "$out/ffmpeg" | tail -n +2 | awk '{print $1}')
  echo "$libs"
  # Only macOS's own libraries and frameworks: nothing from Homebrew or from this build tree.
  if echo "$libs" | grep -vE '^(/usr/lib/|/System/Library/)' | grep -q .; then
    echo "ffmpeg links something that is not part of macOS:" >&2
    echo "$libs" | grep -vE '^(/usr/lib/|/System/Library/)' >&2
    exit 1
  fi
  echo "$(vtool -show-build "$out/ffmpeg" | grep -E 'minos|platform' | tr '\n' ' ')"
  local enc
  enc=$("$out/ffmpeg" -hide_banner -encoders 2>/dev/null)
  for e in h264_videotoolbox hevc_videotoolbox libvpx-vp9 libopus libmp3lame libvorbis aac flac mpeg4 wmv2 gif png; do
    echo "$enc" | grep -qw "$e" || { echo "ffmpeg has no $e encoder" >&2; exit 1; }
  done
  echo "encoders ok"
}

write_notices() {
  local f=$out/FFMPEG-LICENSE.txt rule s
  rule=$(printf '=%.0s' $(seq 1 78))
  : >"$f"
  for s in "${sources[@]}" "${tarballs[@]}"; do
    local name files l path lines
    name=$(field "$s" 0)
    files=$(field "$s" "$([ "$name" = lame ] && echo 3 || echo 4)")
    for l in $files; do
      path=$src/$name/$l
      [ -f "$path" ] || { echo "no licence file $l in $name" >&2; exit 1; }
      printf '%s\n%s (%s)\n%s\n' "$rule" "$name" "$l" "$rule" >>"$f"
      cat "$path" >>"$f"
      printf '\n' >>"$f"
    done
  done

  local version
  version=$(field "${sources[0]}" 2)
  {
    echo "ffmpeg is FFmpeg $version, built from unmodified sources and licensed under the GNU Lesser"
    echo "General Public License version 2.1 or later (FFMPEG-LICENSE.txt). It contains no GPL or"
    echo "non-free parts. The libraries linked into it are listed below with their licences."
    echo
    echo "Build script: shiftdrop-macos/scripts/build-ffmpeg-macos.sh and scripts/ffmpeg-components.txt in"
    echo "https://github.com/CoPrime69/ShiftDrop (this version's source code)."
    echo "Complete corresponding source: ffmpeg-sources.tar.xz (next to this file), or each project at the"
    echo "exact commit below."
    echo
    echo "Sources:"
    for s in "${sources[@]}"; do
      printf '  %-10s %s %s (commit %s)\n' "$(field "$s" 0)" "$(field "$s" 1)" "$(field "$s" 2)" "$(field "$s" 3)"
    done
    for s in "${tarballs[@]}"; do
      printf '  %-10s %s (SHA-256 %s)\n' "$(field "$s" 0)" "$(field "$s" 1)" "$(field "$s" 2)"
    done
    echo
    echo "Toolchain: $(clang --version | head -1); built for arm64, macOS $MIN_MACOS or later, statically linked."
    echo
    echo "FFmpeg configure options:"
    sed -e "s#$work#<work>#g" -e 's/^/  /' "$work/configure-flags.txt"
  } >"$out/FFMPEG-SOURCE.txt"
}

pack_sources() {
  log "corresponding source"
  local names=() s
  for s in "${sources[@]}"; do names+=("$(field "$s" 0)"); done
  for s in "${tarballs[@]}"; do names+=("$(field "$s" 0)"); done
  tar -C "$src" --exclude=.git --exclude=_build -cf - "${names[@]}" \
    -C "$here" build-ffmpeg-macos.sh -C "$repo/scripts" ffmpeg-components.txt | xz -T0 -6 >"$out/ffmpeg-sources.tar.xz"
}

fetch
rm -f "$out/ffmpeg" "$out/ffmpeg-sources.tar.xz"
[ "${SKIP_SOURCES:-0}" = 1 ] || pack_sources   # before anything is built in the source trees
if [ "${SKIP_DEPS:-0}" != 1 ] || [ ! -f "$prefix/lib/libvpx.a" ]; then build_deps; fi
build_ffmpeg
check_binary
write_notices
ls -l "$out"
