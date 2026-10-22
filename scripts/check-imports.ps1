# Fails if an exe imports a networking DLL. ShiftDrop.exe and the archive worker are offline by design;
# this keeps them that way as dependencies are added. ShiftDrop-Updater.exe is the one program allowed
# to go online (check for updates): -Updater checks that it imports WinHTTP and no other networking DLL.
# Run from a Developer PowerShell (needs dumpbin on PATH).
param(
  [Parameter(Mandatory = $true)][string]$Exe,
  [switch]$Updater
)

$banned = @(
  'ws2_32.dll', 'wsock32.dll', 'mswsock.dll', 'wininet.dll', 'winhttp.dll', 'urlmon.dll',
  'dnsapi.dll', 'iphlpapi.dll', 'netapi32.dll', 'websocket.dll', 'webio.dll', 'httpapi.dll',
  'rasapi32.dll', 'wsnmp32.dll'
)
# The updater's one exception, and only for that exe.
$allowed = @()
if ($Updater) {
  if ((Split-Path $Exe -Leaf) -ne 'ShiftDrop-Updater.exe') { throw "-Updater is only for ShiftDrop-Updater.exe, not $Exe" }
  $allowed = @('winhttp.dll')
}

if (-not (Test-Path $Exe)) { throw "Not found: $Exe" }
$output = & dumpbin /nologo /dependents $Exe 2>&1
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed: $output" }

$imports = $output | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ -like '*.dll' }
Write-Host "Imports of $($Exe):"
$imports | ForEach-Object { Write-Host "  $_" }

$hits = $imports | Where-Object { ($banned -contains $_) -and -not ($allowed -contains $_) }
if ($hits) {
  Write-Error "Networking DLLs imported: $($hits -join ', ')"
  exit 1
}
if ($Updater) {
  if (-not ($imports -contains 'winhttp.dll')) {
    Write-Error 'The updater does not import winhttp.dll: its networking is not where this check expects it.'
    exit 1
  }
  Write-Host 'OK: the updater imports winhttp.dll and no other networking DLL.'
} else {
  Write-Host 'OK: no networking DLLs imported.'
}
