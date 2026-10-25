# Fails when anything that states ShiftDrop's version disagrees with VERSION, the one place it is
# written, under the version scheme (docs/BUILDING.md, "Version numbers"). CI runs it on every build;
# run it by hand the same way. Every check is optional except the first two:
#   - VERSION holds one version like 0.7.0, and vcpkg.json states none of its own (it needs none).
#   - -Exe: the exe's version resource states what scripts/make-version-header.cmake makes of VERSION
#     and git for this build (run here again with the same -Release and -Commit, so it needs cmake, and
#     git with the tags): ProductVersion "0.7.0" for the release build (-Release), else a pre-release
#     of the next version, "0.7.1-dev.3+af953b2"; the four numbers and the FileVersion string are the
#     last release and the build number, "0.7.0.3" ("0.7.0.0" for the release).
#   - -Setup: the installer is named ShiftDrop-Setup-<that ProductVersion>.exe and states it too, with
#     the same four numbers.
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
Check ($base -match '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$' -and @($lines | Where-Object { "$_".Trim() }).Count -eq 1) "VERSION holds one version ($base)"
$parts = $base.Split('.')
$nextPatch = "$($parts[0]).$($parts[1]).$([int]$parts[2] + 1)"
$manifest = Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json
$stated = @('version', 'version-string', 'version-semver', 'version-date') | Where-Object { $manifest.PSObject.Properties[$_] }
Check (-not $stated) "vcpkg.json states no version of its own (VERSION is the only place)"

if ($Exe -or $Setup) {
    # What this build must state, from the one script that decides it (the build ran it the same way).
    $header = Join-Path ([IO.Path]::GetTempPath()) "shiftdrop-check-version-$PID.h"
    $releaseOption = if ($Release) { 'ON' } else { 'OFF' }
    & cmake "-DSOURCE_DIR=$root" "-DOUT=$header" "-DRELEASE=$releaseOption" "-DCOMMIT=$Commit" -P (Join-Path $PSScriptRoot 'make-version-header.cmake')
    if ($LASTEXITCODE) { throw "make-version-header.cmake failed ($LASTEXITCODE)" }
    $text = Get-Content $header -Raw
    Remove-Item $header
    function HeaderValue([string]$name) {
        $m = [regex]::Match($text, "(?m)^#define $name ""([^""]*)""")
        if (-not $m.Success) { throw "Version.h has no $name" }
        $m.Groups[1].Value
    }
    $display = HeaderValue 'SD_VERSION_A'
    $numbers = HeaderValue 'SD_FILEVERSION_A'

    # And that is the scheme: the release build is VERSION and VERSION.0; any other build is
    # <VERSION or the patch after it>-dev.<N>+<commit or dev>, with N as its fourth number.
    if ($Release) {
        Check ($display -eq $base -and $numbers -eq "$base.0") "the release build is $base ($numbers)"
    } else {
        $m = [regex]::Match($display, '^(\d+\.\d+\.\d+)-dev\.(0|[1-9]\d*)\+([0-9a-f]{7}|dev)$')
        $ok = $m.Success -and ($m.Groups[1].Value -eq $base -or $m.Groups[1].Value -eq $nextPatch) -and $numbers.EndsWith(".$($m.Groups[2].Value)")
        if ($Commit) { $ok = $ok -and $m.Groups[3].Value -eq $Commit.Substring(0, [Math]::Min(7, $Commit.Length)).ToLowerInvariant() }
        Check $ok "this build is $display ($numbers): a pre-release of $base or $nextPatch, with its build number and commit"
    }
}

function FileNumbers($info) { "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)" }
function ProductNumbers($info) { "$($info.ProductMajorPart).$($info.ProductMinorPart).$($info.ProductBuildPart).$($info.ProductPrivatePart)" }

if ($Exe) {
    $name = Split-Path $Exe -Leaf
    $info = (Get-Item $Exe).VersionInfo
    Check ((FileNumbers $info) -eq $numbers) "$name file version $(FileNumbers $info) is $numbers"
    Check ((ProductNumbers $info) -eq $numbers) "$name product version numbers $(ProductNumbers $info) are $numbers"
    Check ($info.FileVersion -eq $numbers) "$name FileVersion string '$($info.FileVersion)' is $numbers"
    Check ($info.ProductVersion -eq $display) "$name ProductVersion '$($info.ProductVersion)' is $display"
    Check ($info.IsPreRelease -eq (-not $Release)) "$name is$(if ($Release) { ' not' }) marked as a pre-release"
}

if ($Setup) {
    $name = Split-Path $Setup -Leaf
    Check ($name -eq "ShiftDrop-Setup-$display.exe") "installer file $name is ShiftDrop-Setup-$display.exe"
    $info = (Get-Item $Setup).VersionInfo
    # Inno Setup pads its version strings with spaces.
    $product = "$($info.ProductVersion)".Trim().Trim([char]0)
    Check ($product -eq $display) "installer ProductVersion '$product' is $display"
    Check ((FileNumbers $info) -eq $numbers) "installer file version $(FileNumbers $info) is $numbers"
}

if ($Tag) {
    Check ($Tag -eq "v$base") "tag $Tag is v$base"
}

if ($Changelog) {
    $heading = Select-String -Path (Join-Path $root 'CHANGELOG.md') -Pattern "^## \[$([regex]::Escape($base))\]" -List
    Check ($null -ne $heading) "CHANGELOG.md has a '## [$base]' heading"
}
