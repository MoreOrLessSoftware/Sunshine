# Run by the Latency Test client command. A second copy couldn't bind the port, so only ever start one.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if (Get-PluginProcess -Names 'latency-test') {
    Write-Output 'latency-test is already running'
    exit 0
}

Start-Process -FilePath $testExe -WorkingDirectory $dataDir
Write-Output 'Started latency-test'
