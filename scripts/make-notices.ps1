# Builds THIRD-PARTY-NOTICES.txt for the installer from the copyright files vcpkg installs.
# Lists only what is linked into ShiftDrop.exe (gtest is test-only and not shipped).
param(
    [string]$Installed = "build/x64-release/vcpkg_installed/x64-windows-static",
    [string]$Out = "build/installer/THIRD-PARTY-NOTICES.txt"
)
$ErrorActionPreference = 'Stop'

$ports = [ordered]@{
    'libwebp'       = 'libwebp (WebP encoding and decoding)'
    'sqlite3'       = 'SQLite (history database)'
    'nlohmann-json' = 'JSON for Modern C++ (recipes)'
    'wil'           = 'Windows Implementation Libraries (WIL)'
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

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Set-Content -Path $Out -Value $text.ToString() -Encoding utf8
Write-Host "Wrote $Out ($($ports.Count) components)"
