# Capture-WindowsBluetoothTrace.ps1  (run as administrator)
#
# Purpose: find out why Windows does not finish connecting its Bluetooth audio to the Audio Brick. Windows has two built-in trace logs
# (Microsoft-Windows-BTH-BTHPORT/HCI and /L2CAP). This script switches them on, asks Windows to connect to the paired device
# (tools/windows_bt_connect.py), waits, switches the logs off again and exports the traces to the output folder so a
# normal user can read them with Get-WinEvent -Path. Nothing else is changed.
#
#   powershell -ExecutionPolicy Bypass -File tools/Capture-WindowsBluetoothTrace.ps1 -OutDir C:\temp\bt
param([string]$OutDir = "$env:TEMP\bt-trace", [string]$Python = 'python', [string]$Repo = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = Join-Path $OutDir 'capture-log.txt'
"Started: $(Get-Date)" | Out-File $log
$logs = 'Microsoft-Windows-BTH-BTHPORT/HCI', 'Microsoft-Windows-BTH-BTHPORT/L2CAP'
try {
    foreach ($l in $logs) { wevtutil sl $l /e:false 2>$null; wevtutil cl $l 2>$null; wevtutil sl $l /e:true /q:true }
    "trace logs on" | Out-File $log -Append
    & $Python (Join-Path $Repo 'tools\windows_bt_connect.py') 2>&1 | Out-File $log -Append
    Start-Sleep -Seconds 25
} catch {
    "FAILED: $($_.Exception.Message)" | Out-File $log -Append
} finally {
    foreach ($l in $logs) { wevtutil sl $l /e:false 2>$null }
    "trace logs off" | Out-File $log -Append
}
foreach ($l in $logs) {
    # export a copy of the trace that an ordinary user can read (the originals are admin-only)
    $name = ($l -replace '/', '_') + '.evtx'
    $dst = Join-Path $OutDir $name
    if (Test-Path $dst) { Remove-Item $dst -Force }
    wevtutil epl $l $dst
    icacls $dst /grant "Everyone:R" | Out-Null
    "exported $l -> $dst" | Out-File $log -Append
}
"DONE" | Out-File $log -Append
