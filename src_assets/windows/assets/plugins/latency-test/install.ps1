# Downloads the latency analyzer's release zip and unpacks latency-test.exe and latency-analyzer.exe into the plugin's
# data directory, and lets a phone on the same network reach the test app. latency-test.exe finds the analyzer next to
# itself.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'common.ps1')

$version = '0.2'
$zipName = "LatencyAnalyzer-$version-windows.zip"
$zipSha256 = '881493E7A67115C14160F3EC9ECCFDA1C42EED83F7AF22A5CDB89F1A52A612C2'
$exes = @('latency-test.exe', 'latency-analyzer.exe')
$port = 48100

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$url = "https://github.com/MoreOrLessSoftware/latency-analyzer/releases/download/v$version/$zipName"
# Expand-Archive only takes names ending .zip
$zip = Join-Path $dataDir "download-$zipName"
$staging = Join-Path $dataDir 'unpack'

$attempts = 3
for ($attempt = 1; $attempt -le $attempts; $attempt++) {
    try {
        Write-Output "Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        break
    } catch {
        if ($attempt -eq $attempts) {
            throw
        }
        Write-Output "Download failed, trying again: $_"
        Start-Sleep -Seconds 3
    }
}

$actual = (Get-FileHash -Path $zip -Algorithm SHA256).Hash
if ($actual -ne $zipSha256) {
    Remove-Item $zip -Force
    throw "Checksum mismatch for ${zipName}: expected $zipSha256, got $actual"
}

if (Test-Path -LiteralPath $staging) {
    Remove-Item -LiteralPath $staging -Recurse -Force
}
Expand-Archive -LiteralPath $zip -DestinationPath $staging
Remove-Item $zip -Force
foreach ($exe in $exes) {
    if (-not (Test-Path -LiteralPath (Join-Path $staging $exe))) {
        Remove-Item -LiteralPath $staging -Recurse -Force
        throw "$zipName doesn't contain $exe"
    }
}

# A copy left running would keep the files locked
Get-PluginProcess | Stop-Process -Force
Start-Sleep -Milliseconds 500

foreach ($exe in $exes) {
    Move-Item -LiteralPath (Join-Path $staging $exe) -Destination (Join-Path $dataDir $exe) -Force
}
Remove-Item -LiteralPath $staging -Recurse -Force

# Only from the local network: anyone who can reach the port can upload to it while the app runs
Remove-FirewallRules
foreach ($protocol in 'TCP', 'UDP') {
    New-NetFirewallRule -DisplayName "Sunshine Latency Test ($protocol)" -Group $firewallGroup -Direction Inbound -Action Allow `
        -Program $testExe -Protocol $protocol -LocalPort $port -RemoteAddress LocalSubnet -Profile Any | Out-Null
}

Write-Output "Installed latency-test $version in $dataDir"
