# Shared by the Latency Test plugin's scripts. Dot-source this file.

$dataDir = $env:SUNSHINE_PLUGIN_DATA_DIR
$testExe = Join-Path $dataDir 'latency-test.exe'
$firewallGroup = 'Sunshine Latency Test'

# The plugin's own running copies of the two programs, leaving alone any started from somewhere else.
function Get-PluginProcess {
    param([string[]]$Names = @('latency-test', 'latency-analyzer'))

    $prefix = $dataDir.TrimEnd('\') + '\'
    Get-Process -Name $Names -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
    }
}

function Remove-FirewallRules {
    Get-NetFirewallRule -Group $firewallGroup -ErrorAction SilentlyContinue | Remove-NetFirewallRule
}
