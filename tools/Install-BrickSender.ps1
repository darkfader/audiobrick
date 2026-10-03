# Install-BrickSender.ps1  (normal user, no administrator needed)
#
# Starts tools/brick_sender.py at every logon, hidden (pythonw, no window). The sender records VB-Cable's "CABLE Output" and plays
# whatever Windows apps send to "Speakers (VB-Audio Virtual Cable)" on the Audio Brick, only while there is sound.
#
#   powershell -ExecutionPolicy Bypass -File tools/Install-BrickSender.ps1                     # install and start now
#   powershell -ExecutionPolicy Bypass -File tools/Install-BrickSender.ps1 -Remove             # stop and remove
#   powershell -ExecutionPolicy Bypass -File tools/Install-BrickSender.ps1 -HostName 192.168.2.40
#
# The Brick's web password is taken from the AUDIOBRICK_PASSWORD environment variable, or asked for, and saved for the sender in
# %APPDATA%\audiobrick\password (a file in your own profile, never in the repository).
param([string]$HostName = 'audiobrick.local', [switch]$Remove)

$ErrorActionPreference = 'Stop'
$task = 'AudioBrickSender'
$repo = Split-Path -Parent $PSScriptRoot

if ($Remove) {
    Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    Get-CimInstance Win32_Process -Filter "Name='pythonw.exe' OR Name='python.exe'" |
        Where-Object { $_.CommandLine -match 'brick_sender' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    'removed'
    return
}

$python = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source
if (-not $python) { throw 'pythonw.exe not found; install Python from https://www.python.org/downloads/ (with the packages: pip install numpy sounddevice)' }

$pw = $env:AUDIOBRICK_PASSWORD
if (-not $pw) {
    $sec = Read-Host 'Audio Brick web password' -AsSecureString
    $pw = [Runtime.InteropServices.Marshal]::PtrToStringAuto([Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec))
}
$dir = Join-Path $env:APPDATA 'audiobrick'
New-Item -ItemType Directory -Force $dir | Out-Null
Set-Content -Path (Join-Path $dir 'password') -Value $pw -NoNewline

$action = New-ScheduledTaskAction -Execute $python -Argument "`"$repo\tools\brick_sender.py`" $HostName" -WorkingDirectory $repo
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -StartWhenAvailable
Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $task
"installed: '$task' starts at logon and is running now (log: $env:TEMP\brick_sender.log)"
