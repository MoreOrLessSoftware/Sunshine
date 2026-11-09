# Runs when Save options changes the 10-bit SDR option. Writes it to the SudoVDA driver's settings and reloads the
# driver so it takes effect. Sunshine only saves the option if this script succeeds, so on failure the driver's
# settings are left as they were.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vdisplay.ps1')

# vdisplay stays running for the length of a stream, and reloading the driver would remove the stream's display.
if (Get-Process -Name 'vdisplay' -ErrorAction SilentlyContinue) {
    throw 'The virtual display is in use. End the stream, then save the options again.'
}

$previousBits = Get-SudoVdaSdrBits
$enabled = $env:SUNSHINE_PLUGIN_OPT_SDR_10BIT -eq 'true'
Set-SudoVdaSdrBits $(if ($enabled) { 10 } else { $null })
Write-Output "Turning 10-bit SDR $(if ($enabled) { 'on' } else { 'off' })"

$devices = @(Get-SudoVdaDevices | Where-Object { $_.Status -eq 'OK' })
if ($devices.Count -eq 0) {
    Write-Output 'No SudoVDA adapter is running, so the setting applies the next time the driver loads'
    exit 0
}

foreach ($device in $devices) {
    Write-Output "Reloading SudoVDA adapter $($device.InstanceId)"
    try {
        Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false
        Enable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false
    }
    catch {
        $reason = $_
        Enable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        Set-SudoVdaSdrBits $previousBits
        throw "Could not reload the driver, so the setting was not changed. Close any other program using it, such as Apollo, and save the options again: $reason"
    }
}
Write-Output 'Driver reloaded'
