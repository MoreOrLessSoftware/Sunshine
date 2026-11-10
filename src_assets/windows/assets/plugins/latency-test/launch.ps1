# Run by the Launch Latency Test button on the plugin's page. A second copy couldn't bind the port, so only ever start
# one. Buttons run as administrator, but the test app has to run as the signed-in user, or the plugin's other scripts
# couldn't find or close it. Explorer always starts programs as the signed-in user.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if (Get-PluginProcess -Names 'latency-test') {
    Write-Output 'latency-test is already running'
    exit 0
}

if (-not (Test-Path -LiteralPath $testExe)) {
    throw "latency-test isn't installed at $testExe; uninstall and reinstall this plugin"
}

Start-Process -FilePath (Join-Path $env:windir 'explorer.exe') -ArgumentList "`"$testExe`""
Write-Output 'Started latency-test'
