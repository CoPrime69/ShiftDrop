#!/usr/bin/env bash
# Builds ShiftDrop's own ffmpeg.exe: FFmpeg and the few libraries it needs, cross-compiled for 64-bit
# Windows with MinGW-w64 on Linux, linked statically into one exe. Only what the app uses is built
# in (scripts/ffmpeg-components.txt), so the exe is a fraction of a full build's size. LGPL only:
# no --enable-gpl, no --enable-nonfree.
#
# CI runs it on ubuntu-24.04 (.github/workflows/ci.yml, job "ffmpeg"); it runs the same on any
# Debian or Ubuntu with these packages:
#   sudo apt-get install mingw-w64 nasm meson ninja-build cmake pkg-config autoconf automake libtool git curl xz-utils
#
#   scripts/build-ffmpeg.sh [work dir] [output dir]     (defaults: build/ffmpeg-work, build/ffmpeg-custom)
#
# Every source is pinned: a git tag and the commit it must resolve to, or a tarball and its SHA-256.
# A moved tag or a changed download stops the build. The output folder gets ffmpeg.exe,
# FFMPEG-LICENSE.txt (FFmpeg's and every library's licence), FFMPEG-SOURCE.txt (versions, sources
# and the exact configure line) and ffmpeg-sources.tar.xz (the complete corresponding source).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
work=$(mkdir -p "${1:-build/ffmpeg-work}" && cd "${1:-build/ffmpeg-work}" && pwd)
out=$(mkdir -p "${2:-build/ffmpeg-custom}" && cd "${2:-build/ffmpeg-custom}" && pwd)
components=$here/ffmpeg-components.txt
host=x86_64-w64-mingw32
src=$work/src
prefix=$work/prefix
jobs=$(nproc)

# name | git repository | tag | commit the tag must point at | licence file(s) in the tree (file@N: its first N lines)
sources=(
  "ffmpeg|https://github.com/FFmpeg/FFmpeg|n8.1.3|1041abdc962f4cc4f394aa8de9dc5236c0c3b9e7|LICENSE.md COPYING.LGPLv2.1"
  "zlib|https://github.com/madler/zlib|v1.3.2|da607da739fa6047df13e66a2af6b8bec7c2a498|LICENSE"
  "dav1d|https://github.com/videolan/dav1d|1.5.4|54706fc6bc0cdecab7e9593974a4039cc038fca7|COPYING"
  "libvpx|https://github.com/webmproject/libvpx|v1.17.0|6df3ec34557879fff673706f4a1d9fbd0f3a6f0e|LICENSE PATENTS"
  "opus|https://github.com/xiph/opus|v1.6.1|22244de5a79bd1d6d623c32e72bf1954b56235be|COPYING"
  "ogg|https://github.com/xiph/ogg|v1.3.6|be05b13e98b048f0b5a0f5fa8ce514d56db5f822|COPYING"
  "vorbis|https://github.com/xiph/vorbis|v1.3.7|0657aee69dec8508a0011f47f3b69d7538e9d262|COPYING"
  "openh264|https://github.com/cisco/openh264|v2.6.0|652bdb7719f30b52b08e506645a7322ff1b2cc6f|LICENSE"
  "libvpl|https://github.com/intel/libvpl|v2.17.0|d77f9195cf495b937631607333288fd917ae8939|LICENSE"
  # NVENC/NVDEC headers: the 11.1 API runs on NVIDIA drivers from 471.41 (2021) on. Newer headers
  # would need newer drivers and add nothing the app uses.
  "nv-codec-headers|https://github.com/FFmpeg/nv-codec-headers|n11.1.5.4|85f332a190555b56a1d5d4c070a076c871c0dcea|include/ffnvcodec/nvEncodeAPI.h@26"
  # AMF headers only (amf/public/include): 1.4.36 is the oldest FFmpeg accepts, so it runs on the
  # most AMD drivers.
  "amf|https://github.com/GPUOpen-LibrariesAndSDKs/AMF|v1.4.36|16f7d73e0b45c473e903e46981ed0b91efc4c091|LICENSE.txt"
)
# name | tarball | SHA-256 | licence file(s)
tarballs=(
  "lame|https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz|ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e|COPYING LICENSE"
)

log() { printf '\n=== %s\n' "$*"; }
field() { local IFS='|'; set -f; local a=($1); set +f; echo "${a[$2]}"; }

fetch() {
  mkdir -p "$src"
  for s in "${sources[@]}"; do
    local name url tag commit
    name=$(field "$s" 0) url=$(field "$s" 1) tag=$(field "$s" 2) commit=$(field "$s" 3)
    if [ "$(git -C "$src/$name" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
      rm -rf "$src/$name"
      log "fetch $name $tag"
      if [ "$name" = amf ]; then   # a large repository: only the headers are needed
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" --filter=blob:none --sparse "$url" "$src/$name"
        git -C "$src/$name" sparse-checkout set amf/public/include
      else
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$url" "$src/$name"
      fi
    fi
    local got
    got=$(git -C "$src/$name" rev-parse HEAD)
    [ "$got" = "$commit" ] || { echo "$name: tag $tag is $got, expected $commit" >&2; exit 1; }
  done
  for t in "${tarballs[@]}"; do
    local name url sha file
    name=$(field "$t" 0) url=$(field "$t" 1) sha=$(field "$t" 2)
    file=$src/$(basename "$url")
    if [ ! -f "$file" ] || [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$sha" ]; then
      log "fetch $name"
      curl -fsSL --retry 5 --retry-all-errors -o "$file" "$url"
    fi
    echo "$sha  $file" | sha256sum -c --quiet - || { echo "$name: SHA-256 mismatch" >&2; rm -f "$file"; exit 1; }
    if [ ! -d "$src/$name" ]; then
      mkdir -p "$src/$name"
      tar -xzf "$file" -C "$src/$name" --strip-components=1
    fi
  done
}

# ---- toolchain -----------------------------------------------------------------------------------

export CC=$host-gcc-posix CXX=$host-g++-posix AR=$host-ar RANLIB=$host-ranlib STRIP=$host-strip
command -v "$CC" >/dev/null || { export CC=$host-gcc CXX=$host-g++; }
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig PKG_CONFIG_PATH=
# Each project's own release optimisation; every function in its own section so the linker drops what
# nothing calls.
export CFLAGS="-ffunction-sections -fdata-sections" CXXFLAGS="-ffunction-sections -fdata-sections"
export CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib"

cmake_toolchain=$work/mingw.cmake
meson_cross=$work/mingw.meson
write_toolchains() {
  cat >"$cmake_toolchain" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_RC_COMPILER $host-windres)
set(CMAKE_FIND_ROOT_PATH $prefix /usr/$host)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
  cat >"$meson_cross" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
windres = '$host-windres'
pkg-config = 'pkg-config'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
}

cmake_build() {   # <source dir> <cmake options...>
  local dir=$1; shift
  rm -rf "$dir/_build"
  cmake -S "$dir" -B "$dir/_build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$cmake_toolchain" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF "$@"
  cmake --build "$dir/_build" -j "$jobs"
  cmake --install "$dir/_build"
}

# ---- libraries -------------------------------------------------------------------------------------

build_deps() {
  rm -rf "$prefix"
  mkdir -p "$prefix/include" "$prefix/lib/pkgconfig"

  log zlib
  (cd "$src/zlib" && make -f win32/Makefile.gcc clean >/dev/null && \
    make -f win32/Makefile.gcc -j "$jobs" PREFIX=$host- CC="$CC" AR="$AR" RC=$host-windres CFLAGS="-O3 $CFLAGS" libz.a && \
    install -m644 zlib.h zconf.h "$prefix/include/" && install -m644 libz.a "$prefix/lib/")

  log dav1d
  rm -rf "$src/dav1d/_build"
  meson setup "$src/dav1d/_build" "$src/dav1d" --cross-file "$meson_cross" --prefix "$prefix" --libdir lib \
    --buildtype release --default-library static -Denable_tools=false -Denable_tests=false -Denable_examples=false
  ninja -C "$src/dav1d/_build" install

  log libvpx
  # VP9 encoding only (FFmpeg's own decoders are faster for VP8/VP9); high bit depth as in the
  # previously shipped build, so 10-bit sources keep their depth in WebM.
  (cd "$src/libvpx" && rm -rf _build && mkdir _build && cd _build && \
    CROSS=$host- CC="$CC" CXX="$CXX" ../configure --target=x86_64-win64-gcc --prefix="$prefix" --as=nasm \
      --disable-shared --enable-static --enable-pic --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-vp8 --disable-vp9-decoder --enable-vp9-highbitdepth --enable-runtime-cpu-detect && \
    make -j "$jobs" && make install)

  log opus
  cmake_build "$src/opus" -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF

  log ogg
  cmake_build "$src/ogg" -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
  log vorbis
  cmake_build "$src/vorbis" -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_TESTING=OFF

  log lame
  (cd "$src/lame" && (make distclean >/dev/null 2>&1 || true) && \
    ./configure --host=$host --prefix="$prefix" --disable-shared --enable-static --disable-frontend --disable-decoder \
      --disable-gtktest --enable-nasm && make -j "$jobs" && make install)

  log openh264
  (cd "$src/openh264" && make clean >/dev/null 2>&1 || true; \
    make -C "$src/openh264" -j "$jobs" OS=mingw_nt ARCH=x86_64 CC="$CC" CXX="$CXX" AR="$AR" \
      PREFIX="$prefix" install-static)

  log libvpl
  cmake_build "$src/libvpl" -DBUILD_TOOLS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DINSTALL_EXAMPLES=OFF -DINSTALL_DEV=ON
  # The static dispatcher is C++ and uses COM: say so to whoever links it.
  sed -i 's/^Libs.private:.*/& -lstdc++ -lole32 -luuid -ladvapi32/' "$prefix/lib/pkgconfig/vpl.pc"
  grep -q '^Libs.private' "$prefix/lib/pkgconfig/vpl.pc" || echo 'Libs.private: -lstdc++ -lole32 -luuid -ladvapi32' >>"$prefix/lib/pkgconfig/vpl.pc"

  log nv-codec-headers
  make -C "$src/nv-codec-headers" PREFIX="$prefix" install

  log amf
  mkdir -p "$prefix/include/AMF"
  cp -r "$src/amf/amf/public/include/." "$prefix/include/AMF/"
}

# ---- FFmpeg ------------------------------------------------------------------------------------------

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

# --enable-<type>=a,b,c for each section of the components file. Patterns are expanded against
# configure's own lists, and a name configure doesn't know stops the build.
component_flags() {
  local section="" line name cname list flags=()
  declare -A lists
  local -A kind=( [encoders]=encoder [decoders]=decoder [muxers]=muxer [demuxers]=demuxer [filters]=filter
                  [parsers]=parser [bsfs]=bsf [protocols]=protocol [indevs]=indev [hwaccels]=hwaccel )
  set -f
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line%%#*}
    [[ $line =~ ^[[:space:]]*$ ]] && continue
    if [[ $line =~ ^\[([a-z]+)\] ]]; then
      section=${BASH_REMATCH[1]}
      [ -n "${kind[$section]:-}" ] || { echo "unknown section [$section] in $components" >&2; exit 1; }
      lists[$section]=$("$src/ffmpeg/configure" --list-$section | tr -s ' \t' '\n' | sort -u)
      continue
    fi
    for name in $line; do
      cname=$(configure_name "$section" "$name")
      if [[ $cname == *'*'* ]]; then
        local re="^${cname//\*/.*}\$" matched
        matched=$(grep -E "$re" <<<"${lists[$section]}" | tr '\n' ',' | sed 's/,$//')
        [ -n "$matched" ] || { echo "$section: $name matches nothing" >&2; exit 1; }
        flags+=("--enable-${kind[$section]}=$matched")
      else
        grep -qx "$cname" <<<"${lists[$section]}" || { echo "$section: configure has no $cname ($name)" >&2; exit 1; }
        flags+=("--enable-${kind[$section]}=$cname")
      fi
    done
  done <"$components"
  set +f
  printf '%s\n' "${flags[@]}"
}

ffmpeg_flags() {
  cat <<EOF
--prefix=$prefix
--arch=x86_64
--target-os=mingw32
--cross-prefix=$host-
--cc=$CC
--cxx=$CXX
--pkg-config=pkg-config
--pkg-config-flags=--static
--extra-cflags=-I$prefix/include
--extra-ldflags=-L$prefix/lib
--extra-ldflags=-static
--extra-ldflags=-Wl,--gc-sections
--enable-static
--disable-shared
--disable-debug
--disable-doc
--disable-ffplay
--disable-ffprobe
--disable-network
--disable-autodetect
--disable-everything
--enable-w32threads
--enable-zlib
--enable-libdav1d
--enable-libvpx
--enable-libopus
--enable-libvorbis
--enable-libmp3lame
--enable-libopenh264
--enable-libvpl
--enable-ffnvcodec
--enable-nvenc
--enable-nvdec
--enable-cuda
--enable-amf
--enable-mediafoundation
--enable-dxva2
--enable-d3d11va
EOF
  component_flags
}

build_ffmpeg() {
  log ffmpeg
  local b=$src/ffmpeg/_build
  rm -rf "$b" && mkdir -p "$b"
  ffmpeg_flags >"$work/configure-flags.txt"
  (cd "$b" && mapfile -t flags <"$work/configure-flags.txt" && ../configure "${flags[@]}") || {
    tail -50 "$b/ffbuild/config.log"; exit 1; }
  make -C "$b" -j "$jobs" ffmpeg.exe
  "$STRIP" -o "$out/ffmpeg.exe" "$b/ffmpeg.exe"
  # No GPL or non-free code may be in it.
  grep -q '^#define CONFIG_GPL 0' "$b/config.h" && grep -q '^#define CONFIG_NONFREE 0' "$b/config.h" || {
    echo "the build is not LGPL-only" >&2; exit 1; }
}

# ---- notices and the corresponding source ------------------------------------------------------------

write_notices() {
  local f=$out/FFMPEG-LICENSE.txt rule
  rule=$(printf '=%.0s' {1..78})
  : >"$f"
  for s in "${sources[@]}" "${tarballs[@]}"; do
    local name files l path lines
    name=$(field "$s" 0)
    files=$(field "$s" "$([ "$name" = lame ] && echo 3 || echo 4)")
    for l in $files; do
      path=$src/$name/${l%@*} lines=
      [[ $l == *@* ]] && lines=${l#*@}
      [ -f "$path" ] || { echo "no licence file $l in $name" >&2; exit 1; }
      printf '%s\n%s (%s)\n%s\n' "$rule" "$name" "${l%@*}" "$rule" >>"$f"
      if [ -n "$lines" ]; then head -n "$lines" "$path" >>"$f"; else cat "$path" >>"$f"; fi
      printf '\n' >>"$f"
    done
  done

  local version
  version=$(field "${sources[0]}" 2)
  {
    echo "ffmpeg.exe is FFmpeg $version, built by ShiftDrop's CI from unmodified sources and licensed under"
    echo "the GNU Lesser General Public License version 2.1 or later (FFMPEG-LICENSE.txt). It contains no"
    echo "GPL or non-free parts. The libraries linked into it are listed below with their licences."
    echo
    echo "Build script: scripts/build-ffmpeg.sh and scripts/ffmpeg-components.txt in"
    echo "https://github.com/CoPrime69/ShiftDrop (this version's source code)."
    echo "Complete corresponding source: ShiftDrop-<version>-ffmpeg-sources.tar.xz, published with each"
    echo "ShiftDrop release on GitHub (and in each CI run's ffmpeg-custom artifact), or from each project"
    echo "at the exact commit below."
    echo
    echo "Sources:"
    for s in "${sources[@]}"; do
      printf '  %-17s %s %s (commit %s)\n' "$(field "$s" 0)" "$(field "$s" 1)" "$(field "$s" 2)" "$(field "$s" 3)"
    done
    for t in "${tarballs[@]}"; do
      printf '  %-17s %s (SHA-256 %s)\n' "$(field "$t" 0)" "$(field "$t" 1)" "$(field "$t" 2)"
    done
    echo
    echo "Toolchain: $($CC --version | head -1); built for x86_64-w64-mingw32, statically linked."
    echo
    echo "FFmpeg configure options:"
    sed -e "s#$work#<work>#g" -e 's/^/  /' "$work/configure-flags.txt"
  } >"$out/FFMPEG-SOURCE.txt"
}

pack_sources() {
  log "corresponding source"
  local names=()
  for s in "${sources[@]}"; do names+=("$(field "$s" 0)"); done
  for t in "${tarballs[@]}"; do names+=("$(field "$t" 0)"); done
  # openh264's res/ is 60 MB of test bitstreams, not needed to build it.
  tar -C "$src" --exclude=.git --exclude=openh264/res --sort=name --owner=0 --group=0 \
    --mtime='2026-01-01 00:00Z' -cf - "${names[@]}" -C "$here" build-ffmpeg.sh ffmpeg-components.txt | xz -T0 -6 >"$out/ffmpeg-sources.tar.xz"
}

write_toolchains
fetch
[ "${SKIP_SOURCES:-0}" = 1 ] || pack_sources   # before anything is built in the source trees
if [ "${SKIP_DEPS:-0}" != 1 ] || [ ! -f "$prefix/lib/libvpx.a" ]; then build_deps; fi
build_ffmpeg
write_notices
ls -l "$out"
