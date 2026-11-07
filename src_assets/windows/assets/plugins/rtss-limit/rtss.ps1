# Shared helpers for reading and writing RTSS profile frame rate limits.
# Dot-source this file; it defines the [RTSS] interop type and the functions below.

$minimumRtssVersion = [version]'7.3.7'

function Find-RtssDirectory {
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Unwinder\RTSS', 'HKLM:\SOFTWARE\Unwinder\RTSS') {
        $dir = (Get-ItemProperty -Path $key -Name InstallDir -ErrorAction SilentlyContinue).InstallDir
        if ($dir -and (Test-Path (Join-Path $dir 'RTSSHooks64.dll'))) {
            return $dir
        }
    }
    $default = Join-Path ${env:ProgramFiles(x86)} 'RivaTuner Statistics Server'
    if (Test-Path (Join-Path $default 'RTSSHooks64.dll')) {
        return $default
    }
    return $null
}

function Get-RtssVersion([string]$dir) {
    # The installer records the release version; RTSS.exe's own version info can lag behind it
    # (RTSS 7.3.7 ships an RTSS.exe that reports 7.3.5).
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Unwinder\RTSS', 'HKLM:\SOFTWARE\Unwinder\RTSS') {
        $recorded = (Get-ItemProperty -Path $key -Name ProductVersion -ErrorAction SilentlyContinue).ProductVersion
        $parsed = $null
        if ($recorded -and [version]::TryParse($recorded, [ref]$parsed)) {
            return $parsed
        }
    }

    $exe = Join-Path $dir 'RTSS.exe'
    if (-not (Test-Path $exe)) {
        return $null
    }
    $info = (Get-Item $exe).VersionInfo
    return [version]::new($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart)
}

function Initialize-Rtss {
    $dir = Find-RtssDirectory
    if (-not $dir) {
        throw 'RivaTuner Statistics Server is not installed'
    }
    if (-not ('RTSS' -as [type])) {
        $hooks = Join-Path $dir 'RTSSHooks64.dll'
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class RTSS
{
    [DllImport(@"$hooks", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    public static extern bool SetProfileProperty(string name, IntPtr data, uint size);

    [DllImport(@"$hooks", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    public static extern bool GetProfileProperty(string name, IntPtr data, uint size);

    [DllImport(@"$hooks", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    public static extern void LoadProfile(string profile);

    [DllImport(@"$hooks", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    public static extern void SaveProfile(string profile);

    [DllImport(@"$hooks", CallingConvention = CallingConvention.StdCall)]
    public static extern void UpdateProfiles();
}
"@
    }
}

# Returns the RTSS file name of a profile: an empty string for Global, otherwise "<name>.exe".
function Get-RtssProfileFile([string]$name) {
    $name = $name.Trim() -replace '(\.exe)?(\.cfg)?$', ''
    if ($name -eq 'Global') {
        return ''
    }
    return "$name.exe"
}

function Get-RtssProfileNames {
    return @($env:SUNSHINE_PLUGIN_OPT_PROFILES -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

function Get-RtssLimit([string]$name) {
    $file = Get-RtssProfileFile $name
    $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
    try {
        [RTSS]::LoadProfile($file)
        $limit = 0
        $denominator = 1
        if ([RTSS]::GetProfileProperty('FramerateLimit', $ptr, 4)) {
            $limit = [Runtime.InteropServices.Marshal]::ReadInt32($ptr)
        }
        if ([RTSS]::GetProfileProperty('FramerateLimitDenominator', $ptr, 4)) {
            $denominator = [Math]::Max(1, [Runtime.InteropServices.Marshal]::ReadInt32($ptr))
        }
        return @{ limit = $limit; denominator = $denominator }
    }
    finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
    }
}

function Set-RtssLimit([string]$name, [int]$limit, [int]$denominator) {
    $file = Get-RtssProfileFile $name
    $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal(4)
    try {
        [RTSS]::LoadProfile($file)
        [Runtime.InteropServices.Marshal]::WriteInt32($ptr, $denominator)
        if (-not [RTSS]::SetProfileProperty('FramerateLimitDenominator', $ptr, 4)) {
            throw "Failed to set FramerateLimitDenominator on RTSS profile '$name'"
        }
        [Runtime.InteropServices.Marshal]::WriteInt32($ptr, $limit)
        if (-not [RTSS]::SetProfileProperty('FramerateLimit', $ptr, 4)) {
            throw "Failed to set FramerateLimit on RTSS profile '$name'"
        }
        [RTSS]::SaveProfile($file)
    }
    finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
    }
}

# Returns the client's frame rate as an RTSS limit: an integer over a denominator, e.g. 5994/100 for 59.94 FPS.
# Uses the thousandths Sunshine passes when it has them, otherwise hundredths, otherwise whole frames.
function Get-RtssClientLimit {
    if ($env:SUNSHINE_CLIENT_FPS_X1000) {
        $fpsX1000 = [long]$env:SUNSHINE_CLIENT_FPS_X1000
    }
    elseif ($env:SUNSHINE_CLIENT_FPS_X100) {
        $fpsX1000 = [long]$env:SUNSHINE_CLIENT_FPS_X100 * 10
    }
    else {
        $fpsX1000 = [long]$env:SUNSHINE_CLIENT_FPS * 1000
    }

    # Use the coarsest denominator that is exact, so whole and hundredths rates are stored as before.
    if ($fpsX1000 % 1000 -eq 0) {
        return @{ limit = [int]($fpsX1000 / 1000); denominator = 1 }
    }
    if ($fpsX1000 % 10 -eq 0) {
        return @{ limit = [int]($fpsX1000 / 10); denominator = 100 }
    }
    return @{ limit = [int]$fpsX1000; denominator = 1000 }
}

function Get-RtssStateFile {
    return Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'previous-limits.json'
}

# Marker recording that do.ps1 started RTSS, so undo.ps1 only closes an RTSS it started.
function Get-RtssStartedFile {
    return Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'started-rtss'
}

function Test-RtssRunning {
    return [bool](Get-Process -Name RTSS -ErrorAction SilentlyContinue)
}
