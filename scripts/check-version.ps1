# Fails when anything that states ShiftDrop's version disagrees with VERSION, the one place it is
# written. CI runs it on every build; run it by hand the same way. Every check is optional except the
# first two:
#   - VERSION holds one version like 0.6.0, and vcpkg.json states none of its own (it needs none).
#   - -Exe: the exe's version resource. Its numbers are VERSION's; its ProductVersion (what the app
#     shows) is VERSION for the release build (-Release) and VERSION+<commit> otherwise, where
#     <commit> is the first 7 characters of -Commit (any 7 hex digits when -Commit isn't given).
#   - -Setup: the installer is named ShiftDrop-Setup-<that ProductVersion>.exe and states it too.
#   - -Tag: the tag being released is v<VERSION>.
#   - -Changelog: CHANGELOG.md has the heading "## [<VERSION>]" for the release notes.
param(
    [string]$Exe,
    [string]$Setup,
    [string]$Commit,
    [switch]$Release,
    [string]$Tag,
    [switch]$Changelog
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

function Check([bool]$ok, [string]$what) {
    if (-not $ok) { throw "FAILED: $what" }
    Write-Host "ok: $what"
}

$lines = @(Get-Content (Join-Path $root 'VERSION'))
$base = "$($lines[0])".Trim()
Check ($base -match '^\d+\.\d+\.\d+$' -and @($lines | Where-Object { "$_".Trim() }).Count -eq 1) "VERSION holds one version ($base)"
$manifest = Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json
$stated = @('version', 'version-string', 'version-semver', 'version-date') | Where-Object { $manifest.PSObject.Properties[$_] }
Check (-not $stated) "vcpkg.json states no version of its own (VERSION is the only place)"

if ($Release) {
    $display = $base
    $pattern = "^$([regex]::Escape($base))$"
} elseif ($Commit) {
    $display = "$base+$($Commit.Substring(0, [Math]::Min(7, $Commit.Length)).ToLowerInvariant())"
    $pattern = "^$([regex]::Escape($display))$"
} else {
    $display = "$base+<commit>"
    $pattern = "^$([regex]::Escape($base))\+([0-9a-f]{7}|dev)$"
}

if ($Exe) {
    $info = (Get-Item $Exe).VersionInfo
    $numbers = "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart)"
    Check ($numbers -eq $base -and $info.FilePrivatePart -eq 0) "$(Split-Path $Exe -Leaf) file version $numbers.$($info.FilePrivatePart) is $base.0"
    $product = "$($info.ProductMajorPart).$($info.ProductMinorPart).$($info.ProductBuildPart)"
    Check ($product -eq $base) "$(Split-Path $Exe -Leaf) product version numbers $product are $base"
    Check ($info.FileVersion -eq $base) "$(Split-Path $Exe -Leaf) FileVersion string '$($info.FileVersion)' is $base"
    Check ($info.ProductVersion -match $pattern) "$(Split-Path $Exe -Leaf) ProductVersion '$($info.ProductVersion)' is $display"
    $display = $info.ProductVersion
}

if ($Setup) {
    $name = Split-Path $Setup -Leaf
    Check ($name -eq "ShiftDrop-Setup-$display.exe") "installer file $name is ShiftDrop-Setup-$display.exe"
    $info = (Get-Item $Setup).VersionInfo
    # Inno Setup pads its version strings with spaces.
    $product = "$($info.ProductVersion)".Trim().Trim([char]0)
    Check ($product -eq $display) "installer ProductVersion '$product' is $display"
    $numbers = "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart)"
    Check ($numbers -eq $base) "installer file version $numbers is $base"
}

if ($Tag) {
    Check ($Tag -eq "v$base") "tag $Tag is v$base"
}

if ($Changelog) {
    $heading = Select-String -Path (Join-Path $root 'CHANGELOG.md') -Pattern "^## \[$([regex]::Escape($base))\]" -List
    Check ($null -ne $heading) "CHANGELOG.md has a '## [$base]' heading"
}
