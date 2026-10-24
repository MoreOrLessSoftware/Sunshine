# Detects the GPU vendors present and prepares a V-Sync backend for each:
#   NVIDIA: downloads vsync-toggle (https://github.com/xanderfrangos/vsync-toggle)
#   AMD:    uses the driver's "Wait for Vertical Refresh" registry setting

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'common.ps1')

$version = '1.1.0'
$url = "https://github.com/xanderfrangos/vsync-toggle/releases/download/v$version/vsynctoggle-$version-x86_64.exe"
$sha256 = 'F69778DCCE9D3D39F9EC00B793378EFA97DC3404BAFD17910D98C37DF0DBACA8'

$gpus = @(Get-CimInstance -ClassName Win32_VideoController | Select-Object -ExpandProperty Name)
Write-Output "GPUs: $($gpus -join ', ')"

$backends = @()

if ($gpus -match 'NVIDIA') {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Write-Output "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $nvidiaExe -UseBasicParsing

    $actual = (Get-FileHash -Path $nvidiaExe -Algorithm SHA256).Hash
    if ($actual -ne $sha256) {
        Remove-Item $nvidiaExe -Force
        throw "Checksum mismatch for vsynctoggle.exe: expected $sha256, got $actual"
    }
    Write-Output "NVIDIA V-Sync setting: $(& $nvidiaExe status)"
    $backends += 'nvidia'
}

if ($gpus -match 'AMD|Radeon') {
    $keys = @(Get-AmdUmdKeys)
    if ($keys.Count -eq 0) {
        Write-Output 'An AMD GPU was found, but not its driver settings in the registry. Is the AMD driver installed?'
    }
    else {
        foreach ($key in $keys) {
            $current = Get-AmdVsync $key
            Write-Output "AMD adapter $(Split-Path (Split-Path $key) -Leaf): Wait for Vertical Refresh = $(if ($null -eq $current) { 'not set' } else { $current })"
        }
        $backends += 'amd'
    }
}

if ($backends.Count -eq 0) {
    Write-Output 'No supported GPU found. This plugin supports NVIDIA and AMD GPUs.'
    exit 1
}

Set-Content -Path $backendsFile -Value $backends
Write-Output "Using: $($backends -join ', ')"
