# RTSS cannot be downloaded automatically, so this only verifies a supported version is installed.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rtss.ps1')

$dir = Find-RtssDirectory
if (-not $dir) {
    Write-Output 'RivaTuner Statistics Server was not found.'
    Write-Output "Install RTSS $minimumRtssVersion or newer from https://www.guru3d.com/download/rtss-rivatuner-statistics-server-download/ and try again."
    exit 1
}

$version = Get-RtssVersion $dir
Write-Output "Found RTSS $version in $dir"
if ($version -and $version -lt $minimumRtssVersion) {
    Write-Output "RTSS $minimumRtssVersion or newer is required. Update RTSS and try again."
    exit 1
}

Initialize-Rtss
$current = Get-RtssLimit 'Global'
Write-Output "Global profile limit is $($current.limit)/$($current.denominator)"
