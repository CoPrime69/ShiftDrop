# Third-party notices

ShiftDrop's own code is under the [MIT licence](LICENSE). It is built with the libraries below, and
the installer ships a separate `ffmpeg.exe`. This page names each one and its licence. The full
licence texts ship with every install:

- `THIRD-PARTY-NOTICES.txt` in the install folder holds the copyright file of every library linked
  into the app. [scripts/make-notices.ps1](scripts/make-notices.ps1) builds it from the files vcpkg
  installs.
- `engines\FFMPEG-LICENSE.txt` and `engines\FFMPEG-SOURCE.txt` hold FFmpeg's licences, the exact
  versions and where the source is.

Nothing here is under the GPL or the AGPL.

## Linked into ShiftDrop.exe and ShiftDropWorker.exe

All of these come from vcpkg ([vcpkg.json](vcpkg.json)) and are linked statically.

| Library | Used for | Licence |
|---|---|---|
| [WIL](https://github.com/microsoft/wil) (Windows Implementation Libraries) | RAII wrappers for Win32 and COM | MIT |
| [SQLite](https://sqlite.org) | The history database | Public domain |
| [JSON for Modern C++](https://github.com/nlohmann/json) | Recipes and tool parameters | MIT |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | WebP in and out, animated WebP | BSD-3-Clause |
| [QPDF](https://github.com/qpdf/qpdf) | Merge, organize and compress PDFs, read their text | Apache-2.0 |
| [zlib](https://zlib.net) | Compressed streams in the PDFs ShiftDrop writes | zlib |
| [libjpeg-turbo](https://libjpeg-turbo.org) | JPEG images inside PDFs (pulled in by QPDF) | IJG, BSD-3-Clause and zlib |
| [libavif](https://github.com/AOMediaCodec/libavif) | AVIF in and out | BSD-2-Clause |
| [libaom](https://aomedia.googlesource.com/aom) | AV1 encoder for AVIF | BSD-2-Clause, plus the AOMedia Patent License 1.0 |
| [dav1d](https://code.videolan.org/videolan/dav1d) | AV1 decoder for AVIF | BSD-2-Clause |
| [libyuv](https://chromium.googlesource.com/libyuv/libyuv) | Colour conversion inside libavif | BSD-3-Clause |
| [LunaSVG](https://github.com/sammycage/lunasvg) | SVG input | MIT |
| [PlutoVG](https://github.com/sammycage/plutovg) | 2D drawing inside LunaSVG | MIT |
| [miniz](https://github.com/richgel999/miniz) | ZIP containers: DOCX, XLSX, ODS, EPUB | MIT |
| [pugixml](https://pugixml.org) | The XML inside those containers | MIT |
| [MD4C](https://github.com/mity/md4c) | Markdown parsing | MIT |
| [libarchive](https://libarchive.org) | ZIP, TAR, 7z and RAR archives | BSD-2-Clause |
| [bzip2](https://sourceware.org/bzip2/) | .bz2 inside archives (used by libarchive) | bzip2 licence (BSD-style) |
| [XZ Utils liblzma](https://tukaani.org/xz/) | 7z and .xz (used by libarchive) | 0BSD |
| [Zstandard](https://github.com/facebook/zstd) | .zst inside archives (used by libarchive) | BSD-3-Clause (zstd is dual-licensed; ShiftDrop uses it under BSD) |

Build and test only, not shipped:

| Library | Used for | Licence |
|---|---|---|
| [GoogleTest](https://github.com/google/googletest) | The unit tests | BSD-3-Clause |

## FFmpeg (`engines\ffmpeg.exe`)

Video and audio go through FFmpeg, which ShiftDrop starts as a separate program. It is not linked
into ShiftDrop.

The installer that CI publishes has ShiftDrop's own small build, made by
[scripts/build-ffmpeg.sh](scripts/build-ffmpeg.sh) from unmodified sources with only the parts listed
in [scripts/ffmpeg-components.txt](scripts/ffmpeg-components.txt). It is configured without
`--enable-gpl`, `--enable-nonfree` or `--enable-version3`, and the script stops if the result is not
LGPL-only. Each release also carries the complete corresponding source as
`ShiftDrop-<version>-ffmpeg-sources.tar.xz`.

| Component | Version | Licence |
|---|---|---|
| [FFmpeg](https://ffmpeg.org) | n8.1.3 | LGPL-2.1-or-later |
| [LAME](https://lame.sourceforge.io) (MP3 encoder) | 3.100 | LGPL-2.0-or-later |
| [zlib](https://zlib.net) | 1.3.2 | zlib |
| [dav1d](https://code.videolan.org/videolan/dav1d) (AV1 decoder) | 1.5.4 | BSD-2-Clause |
| [libvpx](https://chromium.googlesource.com/webm/libvpx) (VP9 for WebM) | 1.17.0 | BSD-3-Clause, plus its patent grant |
| [Opus](https://opus-codec.org) | 1.6.1 | BSD-3-Clause |
| [libogg](https://xiph.org/ogg/) | 1.3.6 | BSD-3-Clause |
| [libvorbis](https://xiph.org/vorbis/) | 1.3.7 | BSD-3-Clause |
| [OpenH264](https://github.com/cisco/openh264) (software H.264 encoder) | 2.6.0 | BSD-2-Clause |
| [Intel libvpl](https://github.com/intel/libvpl) (Quick Sync) | 2.17.0 | MIT |
| [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) (NVENC/NVDEC headers) | 11.1.5.4 | MIT |
| [AMF headers](https://github.com/GPUOpen-LibrariesAndSDKs/AMF) (AMD encoder headers) | 1.4.36 | MIT |

When no custom build is at hand, [scripts/get-ffmpeg.ps1](scripts/get-ffmpeg.ps1) falls back to a
pinned, unmodified `win64-lgpl` build from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds).
That build is FFmpeg under the LGPL version 3 with no GPL parts. The script writes its licence and
a pointer to its exact source next to the exe.

## Windows components

ShiftDrop also uses parts of Windows that it does not ship: WIC, Direct2D, DirectWrite, Media
Foundation, XAudio2, Windows.Data.Pdf, Windows.Media.Ocr, Windows.Media.FaceAnalysis and Microsoft
Print to PDF. HEIC goes through Windows' own HEIF codec when it is installed. Microsoft Office and
LibreOffice are used only when they are already installed, and are never bundled.
