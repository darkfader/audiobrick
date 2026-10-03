# Set-SingleBrickAudioDevice.ps1  (run as administrator)
#
# Purpose: leave ONE playback device in Windows for the Audio Brick: VB-Cable's "CABLE Input" (listed as "Speakers (VB-Audio Virtual Cable)").
#          VB-Cable also installs a second, 16-channel playback device ("CABLE In 16 Ch"); this script hides it. Nothing is uninstalled.
#          Your own devices and everything not made by VB-Audio are never touched.
#
#   powershell -ExecutionPolicy Bypass -File tools/Set-SingleBrickAudioDevice.ps1            # make sure the main cable devices are on, hide the 16-channel one
#   powershell -ExecutionPolicy Bypass -File tools/Set-SingleBrickAudioDevice.ps1 -Restore   # show the 16-channel device again
#
# History: this used to hide the ~18 extra devices that Voicemeeter creates. That did NOT work on the PC it was tried on: after hiding
# any VB-Audio device (even only the cable ones) Voicemeeter received no audio from "Voicemeeter Input" until everything was restored and the
# Windows Audio service restarted. Hence the VB-Cable route in docs/windows-setup.md. Hiding uses the audio endpoint API
# (IPolicyConfig::SetEndpointVisibility, what the Sound control panel uses); Disable-PnpDevice does nothing for audio endpoints.
param([switch]$Restore)

$ErrorActionPreference = 'Stop'
$out = Join-Path $env:TEMP 'brick-audio-devices-log.txt'
"Started: $(Get-Date)  Restore=$Restore" | Out-File $out

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("f8679f50-850a-41cf-9c72-430f290290c8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPolicyConfig {
    [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string d, IntPtr f);
    [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string d, [MarshalAs(UnmanagedType.Bool)] bool def, IntPtr f);
    [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string d);
    [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string d, IntPtr a, IntPtr b);
    [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string d, [MarshalAs(UnmanagedType.Bool)] bool def, IntPtr a, IntPtr b);
    [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string d, IntPtr a);
    [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string d, IntPtr m);
    [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string d, IntPtr m);
    [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string d, [MarshalAs(UnmanagedType.Bool)] bool fx, IntPtr key, IntPtr v);
    [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string d, [MarshalAs(UnmanagedType.Bool)] bool fx, IntPtr key, IntPtr v);
    [PreserveSig] int SetDefaultEndpoint([MarshalAs(UnmanagedType.LPWStr)] string d, int role);
    [PreserveSig] int SetEndpointVisibility([MarshalAs(UnmanagedType.LPWStr)] string d, [MarshalAs(UnmanagedType.Bool)] bool visible);
}
[ComImport, Guid("870af99c-171d-4f9e-af0d-e63df40c2bc9")] class PolicyConfigClient { }
public static class EndpointVisibility {
    public static int Set(string id, bool visible) {
        var p = (IPolicyConfig)new PolicyConfigClient();
        return p.SetEndpointVisibility(id, visible);
    }
}
'@

function Set-Endpoint([string]$id, [bool]$visible) {
    $hr = [EndpointVisibility]::Set($id, $visible)
    if ($hr -ne 0) { throw ('SetEndpointVisibility failed, HRESULT 0x' + $hr.ToString('X8')) }  # no -f: the id contains braces
}

function Get-Endpoints {
    # Windows' own names of the endpoints, from the MMDevices registry: Render = playback, Capture = recording.
    foreach ($flow in 'Render', 'Capture') {
        $base = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$flow"
        foreach ($k in Get-ChildItem $base) {
            $props = Get-ItemProperty "$($k.PSPath)\Properties" -ErrorAction SilentlyContinue
            [pscustomobject]@{
                Flow = $flow
                # the API wants the full endpoint id: {0.0.0.00000000}.{GUID} for playback, {0.0.1.00000000}.{GUID} for recording
                Id   = $(if ($flow -eq 'Render') { '{0.0.0.00000000}.' } else { '{0.0.1.00000000}.' }) + $k.PSChildName
                Name = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
                Adapter = $props.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
            }
        }
    }
}

try {
    $cable = @(Get-Endpoints | Where-Object { $_.Adapter -match 'VB-Audio Virtual Cable' })
    if ($cable.Count -eq 0) { throw 'no VB-Cable devices found; install VB-Cable first (https://vb-audio.com/Cable/)' }
    foreach ($e in $cable) {
        $twin = $e.Flow -eq 'Render' -and $e.Name -match '16 Ch'
        if ($Restore -or -not $twin) {
            Set-Endpoint $e.Id $true
            "enabled  $($e.Flow): $($e.Name)" | Out-File $out -Append
        } else {
            Set-Endpoint $e.Id $false
            "disabled $($e.Flow): $($e.Name)" | Out-File $out -Append
        }
    }
    "SUCCESS" | Out-File $out -Append
} catch {
    "FAILED: $($_.Exception.Message)" | Out-File $out -Append
}
