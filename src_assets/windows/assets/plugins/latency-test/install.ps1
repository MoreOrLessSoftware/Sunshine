# Downloads latency-test.exe and latency-analyzer.exe into the plugin's data directory, and lets a phone on the same
# network reach the test app. latency-test.exe finds the analyzer next to itself.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'common.ps1')

$version = 'v0.1'
$port = 48100
$files = @(
    @{ Name = 'latency-test.exe'; Sha256 = 'C9B0EF13188817B2357E6504ED19E7D3BF7F2F524FBBF45B4B7665D56CAA3D6D' },
    @{ Name = 'latency-analyzer.exe'; Sha256 = '53178B78CBCCD742E636F88512C4701BDB3AB3F4C75518AAE3234447941931CE' }
)

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Get-Release {
    param([string]$Name, [string]$Sha256)

    $url = "https://github.com/MoreOrLessSoftware/latency-analyzer/releases/download/$version/$Name"
    $partial = Join-Path $dataDir "$Name.download"
    $attempts = 3
    for ($attempt = 1; $attempt -le $attempts; $attempt++) {
        try {
            Write-Output "Downloading $url"
            Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing
            break
        } catch {
            if ($attempt -eq $attempts) {
                throw
            }
            Write-Output "Download failed, trying again: $_"
            Start-Sleep -Seconds 3
        }
    }

    $actual = (Get-FileHash -Path $partial -Algorithm SHA256).Hash
    if ($actual -ne $Sha256) {
        Remove-Item $partial -Force
        throw "Checksum mismatch for ${Name}: expected $Sha256, got $actual"
    }
    Move-Item -Path $partial -Destination (Join-Path $dataDir $Name) -Force
}

# A copy left running would keep the files locked
Get-PluginProcess | Stop-Process -Force
Start-Sleep -Milliseconds 500

foreach ($file in $files) {
    Get-Release -Name $file.Name -Sha256 $file.Sha256
}

# Only from the local network: anyone who can reach the port can upload to it while the app runs
Remove-FirewallRules
foreach ($protocol in 'TCP', 'UDP') {
    New-NetFirewallRule -DisplayName "Sunshine Latency Test ($protocol)" -Group $firewallGroup -Direction Inbound -Action Allow `
        -Program $testExe -Protocol $protocol -LocalPort $port -RemoteAddress LocalSubnet -Profile Any | Out-Null
}

Write-Output "Installed latency-test $version in $dataDir"
