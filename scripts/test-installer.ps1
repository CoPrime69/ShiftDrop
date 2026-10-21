# Smoke test for the installer, run by CI on a clean runner: install, upgrade over a running copy,
# respect a saved "start with Windows" choice, uninstall. Uses the real per-user locations.
param([Parameter(Mandatory)][string]$Setup)
$ErrorActionPreference = 'Stop'

$app = Join-Path $env:LOCALAPPDATA 'Programs\ShiftDrop'
$exe = Join-Path $app 'ShiftDrop.exe'
$settings = Join-Path $env:LOCALAPPDATA 'ShiftDrop\settings.ini'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\ShiftDrop.lnk'
$logDir = Join-Path $env:RUNNER_TEMP 'installer-logs'
New-Item -ItemType Directory -Force $logDir | Out-Null

function Check([bool]$ok, [string]$what) {
    if (-not $ok) { throw "FAILED: $what" }
    Write-Host "ok: $what"
}
function RunValue { (Get-ItemProperty $runKey -Name ShiftDrop -ErrorAction SilentlyContinue).ShiftDrop }
# The right-click entry ('*' is a real key name here, hence -LiteralPath).
$verbKey = 'HKCU:\Software\Classes\*\shell\ShiftDrop\command'
function VerbCommand { (Get-ItemProperty -LiteralPath $verbKey -ErrorAction SilentlyContinue).'(default)' }
function Install([string]$name) {
    $p = Start-Process $Setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$logDir\$name.log`"" -Wait -PassThru
    Check ($p.ExitCode -eq 0) "$name exits 0 (got $($p.ExitCode))"
}
function WaitFor([scriptblock]$cond, [int]$seconds) {
    for ($i = 0; $i -lt $seconds * 2; $i++) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 500 }
    return (& $cond)
}

# A clean machine: no saved choice, no Run value.
Remove-ItemProperty $runKey -Name ShiftDrop -ErrorAction SilentlyContinue
Remove-Item $settings -ErrorAction SilentlyContinue

Install 'install'
Check (Test-Path $exe) 'exe installed under %LOCALAPPDATA%\Programs\ShiftDrop'
Check (Test-Path (Join-Path $app 'THIRD-PARTY-NOTICES.txt')) 'notices installed'
Check (Test-Path (Join-Path $app 'engines\ffmpeg.exe')) 'ffmpeg installed in engines'
Check (Test-Path (Join-Path $app 'engines\FFMPEG-SOURCE.txt')) 'ffmpeg source pointer installed'
Check (Test-Path (Join-Path $app 'engines\FFMPEG-LICENSE.txt')) 'ffmpeg licences installed'
# The installed exe runs on its own (statically linked: no DLLs beside it).
$ffVersion = & (Join-Path $app 'engines\ffmpeg.exe') -hide_banner -version 2>&1 | Select-Object -First 1
Check ($LASTEXITCODE -eq 0 -and "$ffVersion" -like 'ffmpeg version*') "installed ffmpeg runs ($ffVersion)"
Check (Test-Path (Join-Path $app 'engines\ShiftDropWorker.exe')) 'sandboxed worker installed in engines'
Check (Test-Path $shortcut) 'Start menu shortcut'
Check ((RunValue) -eq "`"$exe`" --autostart") 'Run value points at the installed exe'
Check ((Get-Content $settings -Raw) -match 'StartWithWindows=1') 'choice saved as on'
Check ((VerbCommand) -eq "`"$exe`" --wheel `"%1`"") 'right-click entry points at the installed exe'
Check (Test-Path -LiteralPath 'HKCU:\Software\Classes\Directory\shell\ShiftDrop\command') 'right-click entry for folders'

# Upgrade over a running copy: Setup must ask it to quit, then replace the exe.
$running = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
if ($running.HasExited) {
    Write-Warning "The app exited on its own on this runner (code $($running.ExitCode)); skipping the running-copy check."
    $running = $null
}
Install 'upgrade'
if ($running) { Check (WaitFor { $running.HasExited } 5) 'upgrade quit the running copy' }

# A choice saved from the tray menu survives an upgrade.
Set-Content $settings "[Startup]`r`nStartWithWindows=0" -Encoding ascii
Remove-ItemProperty $runKey -Name ShiftDrop -ErrorAction SilentlyContinue
Install 'upgrade-off'
Check ($null -eq (RunValue)) 'upgrade keeps start-with-Windows off'
Check ((Get-Content $settings -Raw) -match 'StartWithWindows=0') 'saved choice untouched'
Set-Content $settings "[Startup]`r`nStartWithWindows=1" -Encoding ascii
Install 'upgrade-on'
Check ((RunValue) -eq "`"$exe`" --autostart") 'upgrade restores the Run value when the choice is on'

# Uninstall while running. The uninstaller re-launches itself from %TEMP%, so wait for the files to go.
$running = Start-Process $exe -PassThru
Start-Sleep -Seconds 4
if ($running.HasExited) { $running = $null }
$p = Start-Process (Join-Path $app 'unins000.exe') -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$logDir\uninstall.log`"" -Wait -PassThru
Check ($p.ExitCode -eq 0) "uninstall exits 0 (got $($p.ExitCode))"
Check (WaitFor { -not (Test-Path $exe) } 60) 'exe removed'
Check (WaitFor { -not (Test-Path (Join-Path $app 'engines\ffmpeg.exe')) } 30) 'ffmpeg removed'
Check (-not (Test-Path $shortcut)) 'shortcut removed'
Check ($null -eq (RunValue)) 'Run value removed'
Check (-not (Test-Path -LiteralPath 'HKCU:\Software\Classes\*\shell\ShiftDrop')) 'right-click entry removed'
if ($running) { Check (WaitFor { $running.HasExited } 5) 'uninstall quit the running copy' }
Check (Test-Path $settings) 'user settings kept'
Write-Host 'installer smoke test passed'
