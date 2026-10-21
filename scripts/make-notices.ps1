# Builds THIRD-PARTY-NOTICES.txt for the installer from the copyright files vcpkg installs.
# Lists what is linked into ShiftDrop.exe and engines\ShiftDropWorker.exe (gtest is test-only and not shipped), plus the bundled ffmpeg.exe.
param(
    [string]$Installed = "build/x64-release/vcpkg_installed/x64-windows-static",
    [string]$Out = "build/installer/THIRD-PARTY-NOTICES.txt",
    [string]$Engines = "build/engines"
)
$ErrorActionPreference = 'Stop'

$ports = [ordered]@{
    'libwebp'       = 'libwebp (WebP encoding and decoding)'
    'sqlite3'       = 'SQLite (history database)'
    'nlohmann-json' = 'JSON for Modern C++ (recipes)'
    'wil'           = 'Windows Implementation Libraries (WIL)'
    'qpdf'          = 'QPDF (PDF merge, organize, compress and text)'
    'zlib'          = 'zlib (PDF compression)'
    'libjpeg-turbo' = 'libjpeg-turbo (used by QPDF for JPEG images)'
    'libavif'       = 'libavif (AVIF encoding and decoding)'
    'aom'           = 'libaom (AV1 encoder for AVIF)'
    'dav1d'         = 'dav1d (AV1 decoder for AVIF)'
    'libyuv'        = 'libyuv (colour conversion, used by libavif)'
    'lunasvg'       = 'LunaSVG (SVG rendering)'
    'plutovg'       = 'PlutoVG (2D vector graphics, used by LunaSVG)'
    'miniz'         = 'miniz (ZIP containers: DOCX, XLSX, ODS, EPUB)'
    'pugixml'       = 'pugixml (XML of documents and spreadsheets)'
    'md4c'          = 'MD4C (Markdown parsing)'
    'libarchive'    = 'libarchive (ZIP, TAR, 7z and RAR archives; in ShiftDropWorker.exe and ShiftDrop.exe)'
    'bzip2'         = 'bzip2 (used by libarchive)'
    'liblzma'       = 'XZ Utils liblzma (used by libarchive for 7z and .xz)'
    'zstd'          = 'Zstandard (used by libarchive)'
}

$text = [System.Text.StringBuilder]::new()
[void]$text.AppendLine('ShiftDrop includes the following third-party software.')
[void]$text.AppendLine()
foreach ($port in $ports.Keys) {
    $file = Join-Path $Installed "share/$port/copyright"
    if (-not (Test-Path $file)) { throw "No copyright file for $port at $file (was the Release build configured?)" }
    [void]$text.AppendLine('=' * 78)
    [void]$text.AppendLine($ports[$port])
    [void]$text.AppendLine('=' * 78)
    [void]$text.AppendLine((Get-Content $file -Raw).TrimEnd())
    [void]$text.AppendLine()
}

# FFmpeg is a separate program (engines\ffmpeg.exe), not linked in; scripts/get-ffmpeg.ps1 writes
# its source pointer and licence next to it.
$ffmpeg = Join-Path $Engines 'FFMPEG-SOURCE.txt'
if (-not (Test-Path $ffmpeg)) { throw "No $ffmpeg (run scripts/get-ffmpeg.ps1 first)" }
[void]$text.AppendLine('=' * 78)
[void]$text.AppendLine('FFmpeg (video and audio conversion, engines\ffmpeg.exe)')
[void]$text.AppendLine('=' * 78)
[void]$text.AppendLine((Get-Content $ffmpeg -Raw).TrimEnd())
[void]$text.AppendLine()
[void]$text.AppendLine((Get-Content (Join-Path $Engines 'FFMPEG-LICENSE.txt') -Raw).TrimEnd())
[void]$text.AppendLine()

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Set-Content -Path $Out -Value $text.ToString() -Encoding utf8
Write-Host "Wrote $Out ($($ports.Count) components + FFmpeg)"
