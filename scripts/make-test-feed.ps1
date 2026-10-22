# Makes a local "release" to test Check for updates before a real newer release exists (see
# docs/TESTING.md, "Check for updates"). It copies an installer (any CI installer works) as
# ShiftDrop-Setup-<Version>.exe into -Out, writes SHA256SUMS.txt for it and a latest.json shaped like
# GitHub's answer whose downloads are those two local files, published -DaysOld days ago.
# Then point the app at it: settings.ini [Updates] TestFeed=<Out>\latest.json, and restart ShiftDrop.
# -BadHash writes a wrong hash, to see the download refused.
param(
    [Parameter(Mandatory)][string]$Setup,
    [string]$Version = '9.9.9',
    [int]$DaysOld = 8,
    [string]$Out = (Join-Path $env:TEMP 'ShiftDrop-test-feed'),
    [switch]$BadHash
)
$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$name = "ShiftDrop-Setup-$Version.exe"
$copy = Join-Path $Out $name
Copy-Item $Setup $copy -Force
$hash = (Get-FileHash $copy -Algorithm SHA256).Hash.ToLowerInvariant()
if ($BadHash) { $hash = ('0' * 63) + '1' }
$sums = Join-Path $Out 'SHA256SUMS.txt'
[IO.File]::WriteAllText($sums, "$hash  $name`n")

$published = (Get-Date).ToUniversalTime().AddDays(-$DaysOld).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")
$release = [ordered]@{
    tag_name     = "v$Version"
    name         = "ShiftDrop $Version"
    draft        = $false
    prerelease   = $false
    published_at = $published
    html_url     = 'https://github.com/CoPrime69/ShiftDrop/releases'
    body         = "A test release made by scripts/make-test-feed.ps1.`n`n## What's in this build`n`n- Nothing new: this is $(Split-Path $Setup -Leaf) under another name"
    assets       = @(
        [ordered]@{ name = $name; size = (Get-Item $copy).Length; browser_download_url = $copy },
        [ordered]@{ name = 'SHA256SUMS.txt'; size = (Get-Item $sums).Length; browser_download_url = $sums }
    )
}
$feed = Join-Path $Out 'latest.json'
[IO.File]::WriteAllText($feed, ($release | ConvertTo-Json -Depth 4))   # UTF-8 without a BOM
Write-Host "Test feed: $feed (v$Version, published $published)"
Write-Host "In %LOCALAPPDATA%\ShiftDrop\settings.ini add:"
Write-Host "  [Updates]"
Write-Host "  TestFeed=$feed"
Write-Host 'then quit and start ShiftDrop again. Remove the line when done.'
