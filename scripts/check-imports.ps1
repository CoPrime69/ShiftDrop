# Fails if ShiftDrop.exe imports any networking DLL. ShiftDrop is offline by design; this keeps it
# that way as dependencies are added. Run from a Developer PowerShell (needs dumpbin on PATH).
param([Parameter(Mandatory = $true)][string]$Exe)

$banned = @(
  'ws2_32.dll', 'wsock32.dll', 'mswsock.dll', 'wininet.dll', 'winhttp.dll', 'urlmon.dll',
  'dnsapi.dll', 'iphlpapi.dll', 'netapi32.dll', 'websocket.dll', 'webio.dll', 'httpapi.dll',
  'rasapi32.dll', 'wsnmp32.dll'
)

if (-not (Test-Path $Exe)) { throw "Not found: $Exe" }
$output = & dumpbin /nologo /dependents $Exe 2>&1
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed: $output" }

$imports = $output | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ -like '*.dll' }
Write-Host "Imports of $($Exe):"
$imports | ForEach-Object { Write-Host "  $_" }

$hits = $imports | Where-Object { $banned -contains $_ }
if ($hits) {
  Write-Error "Networking DLLs imported: $($hits -join ', ')"
  exit 1
}
Write-Host 'OK: no networking DLLs imported.'
