# Downloads the pinned LGPL build of FFmpeg and puts ffmpeg.exe (plus its licence) in build/engines.
# CI runs it before the build; run it once by hand for a local build (docs/TESTING.md, section 5).
# The zip is checked against a pinned SHA-256, so a changed or tampered download fails the build.
#
# Build: BtbN/FFmpeg-Builds (https://github.com/BtbN/FFmpeg-Builds), "win64-lgpl" variant: no GPL
# parts (no x264/x265), so ShiftDrop can ship it unmodified next to its own exe. To update, pick a
# newer win64-lgpl zip from that project's releases and change the three values below.
param(
    [string]$Out = "build/engines",
    [string]$Downloads = "build/downloads"
)
$ErrorActionPreference = 'Stop'

$Url = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-20-13-08/ffmpeg-n8.1.3-18-g46d899d7a0-win64-lgpl-8.1.zip'
$Sha256 = '26634a5e06429c4f51ed08ba518923f928c5e4a49ed0ee62f701e035227b91a7'
$Version = 'n8.1.3-18-g46d899d7a0'   # FFmpeg's own version string for this build (see NOTICE below)

$zipName = Split-Path $Url -Leaf
$zip = Join-Path $Downloads $zipName
$exe = Join-Path $Out 'ffmpeg.exe'
$stamp = Join-Path $Out 'ffmpeg.version'

if ((Test-Path $exe) -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $Sha256)) {
    Write-Host "ffmpeg $Version already in $Out"
    exit 0
}

New-Item -ItemType Directory -Force $Downloads, $Out | Out-Null
if (-not (Test-Path $zip) -or (Get-FileHash $zip -Algorithm SHA256).Hash -ne $Sha256) {
    Write-Host "Downloading $zipName"
    $ProgressPreference = 'SilentlyContinue'   # the progress bar makes Invoke-WebRequest very slow
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
}
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
if ($hash -ne $Sha256) {
    Remove-Item $zip
    throw "SHA-256 mismatch for $zipName`n expected $Sha256`n got      $hash"
}

# Only two files are needed from the 170 MB zip: the exe and the licence text.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $zip))
try {
    foreach ($want in @{ 'bin/ffmpeg.exe' = $exe; 'LICENSE.txt' = (Join-Path $Out 'FFMPEG-LICENSE.txt') }.GetEnumerator()) {
        $entry = $archive.Entries | Where-Object { $_.FullName -like "*/$($want.Key)" } | Select-Object -First 1
        if (-not $entry) { throw "$($want.Key) not found in $zipName" }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path (Resolve-Path $Out) (Split-Path $want.Value -Leaf)), $true)
    }
}
finally {
    $archive.Dispose()
}
Set-Content -Path $stamp -Value $Sha256 -Encoding ascii
Set-Content -Path (Join-Path $Out 'FFMPEG-SOURCE.txt') -Encoding utf8 -Value @"
ffmpeg.exe is FFmpeg $Version, built by BtbN/FFmpeg-Builds (win64-lgpl), licensed under the
GNU Lesser General Public License version 3 (FFMPEG-LICENSE.txt). It is shipped unmodified.

Build used:   $Url
FFmpeg source for this exact version: https://github.com/FFmpeg/FFmpeg/tree/$($Version.Split('-')[-1].TrimStart('g'))
Build scripts (which libraries, which options): https://github.com/BtbN/FFmpeg-Builds
"@
Write-Host "ffmpeg $Version -> $exe"
