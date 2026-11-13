# Lists the connected physical displays for the display option, the primary one first, then the others that are on,
# then the ones that are off. Each is chosen by its device path, which stays the same across restarts.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'display.ps1')

$displays = @(Get-PhysicalDisplays | Where-Object DevicePath |
    Sort-Object @{ Expression = { -not $_.Primary } }, @{ Expression = { -not $_.Active } }, @{ Expression = { Get-DisplayLabel $_ } })

# Displays of the same model get numbers, so they can be told apart.
$counts = @{}
foreach ($display in $displays) {
    $counts[(Get-DisplayLabel $display)]++
}
$seen = @{}
$choices = foreach ($display in $displays) {
    $label = Get-DisplayLabel $display
    if ($counts[$label] -gt 1) {
        $seen[$label]++
        $label = "$label $($seen[$label])"
    }
    $details = @()
    $connection = Get-ConnectionName $display.OutputTechnology
    if ($connection -and $connection -notlike 'built-in*') {
        $details += $connection
    }
    if ($display.Primary) {
        $details += 'primary'
    }
    elseif (-not $display.Active) {
        $details += 'off'
    }
    if ($details.Count -gt 0) {
        $label = "$label ($($details -join ', '))"
    }
    [ordered]@{ value = $display.DevicePath; label = $label }
}

ConvertTo-Json -InputObject @($choices) -Compress | Set-Content -LiteralPath $env:SUNSHINE_PLUGIN_CHOICES_FILE -Encoding UTF8
Write-Output "Listed $(@($choices).Count) displays"
