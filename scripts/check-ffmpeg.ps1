# Asks a bundled ffmpeg.exe what it was built with and fails if anything ShiftDrop needs is missing:
# every encoder, decoder, muxer, demuxer, filter, protocol and input device in
# scripts/ffmpeg-components.txt (names with * are patterns and are not checked one by one), plus the
# GPU decoders behind -hwaccel auto. tests/FfmpegComponentsTests.cpp checks that the file lists
# everything the app's argument builders use; this checks that the exe matches the file.
# CI runs it on whichever ffmpeg.exe goes into the installer (our own build or the pinned BtbN one).
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Components = (Join-Path $PSScriptRoot 'ffmpeg-components.txt')
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "Not found: $Exe" }

# Section -> names, from the components file.
$want = @{}
$section = $null
foreach ($raw in Get-Content $Components) {
    $line = ($raw -replace '#.*$', '').Trim()
    if (-not $line) { continue }
    if ($line -match '^\[(\w+)\]') { $section = $Matches[1]; $want[$section] = @(); continue }
    $want[$section] += ($line -split '\s+' | Where-Object { $_ -and $_ -notlike '*`**' })
}

function Ask([string]$what) {
    $text = & $Exe -hide_banner $what 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg $what failed: $($text -join "`n")" }
    return $text
}
# Names after the "---" line: " V....D libvpx-vp9  ..." / "  E mp4  ..." / " ... scale  V->V ...";
# formats can be listed together ("mov,mp4,m4a,3gp,3g2,mj2").
function Listed([string[]]$text, [string]$pattern) {
    $names = [System.Collections.Generic.HashSet[string]]::new()
    $started = $false
    foreach ($l in $text) {
        if (-not $started) { if ($l -match '^\s*-{2,}\s*$') { $started = $true }; continue }
        if ($l -match $pattern) { foreach ($n in $Matches[1] -split ',') { [void]$names.Add($n) } }
    }
    return $names
}

$have = @{
    encoders = Listed (Ask '-encoders') '^\s[A-Z\.]{6}\s+(\S+)'
    decoders = Listed (Ask '-decoders') '^\s[A-Z\.]{6}\s+(\S+)'
    muxers   = Listed (Ask '-muxers') '^\s*[DEd\. ]{1,3}\s+(\S+)'
    demuxers = Listed (Ask '-demuxers') '^\s*[DEd\. ]{1,3}\s+(\S+)'
    indevs   = Listed (Ask '-devices') '^\s*[DEd\. ]{1,3}\s+(\S+)'
    filters  = Listed (Ask '-filters') '^\s[TSC\.]{2,3}\s+(\S+)\s+\S*->\S*'
}
$protocols = Ask '-protocols'
$inputs = $protocols[($protocols.IndexOf('Input:') + 1)..($protocols.IndexOf('Output:') - 1)] | ForEach-Object { $_.Trim() }
$have.protocols = [System.Collections.Generic.HashSet[string]]::new([string[]]$inputs)
$have.hwaccels = [System.Collections.Generic.HashSet[string]]::new([string[]]((Ask '-hwaccels') | Select-Object -Skip 1 | ForEach-Object { $_.Trim() } | Where-Object { $_ }))

$missing = @()
foreach ($s in 'encoders', 'decoders', 'muxers', 'demuxers', 'filters', 'protocols', 'indevs') {
    if (-not $want.ContainsKey($s)) { continue }
    $gone = @($want[$s] | Where-Object { -not $have[$s].Contains($_) })
    Write-Host ("{0,-9} {1,4} listed, {2,4} in the exe{3}" -f $s, $want[$s].Count, $have[$s].Count, $(if ($gone) { ", missing: $($gone -join ' ')" } else { '' }))
    $missing += $gone | ForEach-Object { "$s/$_" }
}
# -hwaccel auto: DXVA2 and D3D11 on any GPU, CUDA (NVDEC) on NVIDIA; QSV for h264_qsv.
foreach ($h in 'dxva2', 'd3d11va', 'cuda', 'qsv') {
    if (-not $have.hwaccels.Contains($h)) { $missing += "hwaccels/$h" }
}
Write-Host "hwaccels  $($have.hwaccels -join ' ')"
Write-Host ("ffmpeg.exe: {0:N1} MB, {1}" -f ((Get-Item $Exe).Length / 1MB), ((& $Exe -hide_banner -version 2>&1 | Select-Object -First 1)))
if ($missing) {
    Write-Error "ffmpeg.exe lacks: $($missing -join ', ')"
    exit 1
}
Write-Host 'OK: ffmpeg.exe has everything in ffmpeg-components.txt.'
