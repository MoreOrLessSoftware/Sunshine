# Lists the installed HDR profiles that state their peak brightness for the profile option, the primary display's
# current one first, then the rest by name. Profiles that laptop makers install for their own panels (TPLCD_*) are left
# out unless they are linked to the display.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

$installed = Get-InstalledHdrProfiles

# Names a profile by its description and peak brightness, falling back to its file name for the description.
function Get-ProfileLabel([string]$name) {
    $hdrProfile = $installed[$name]
    $label = if ($hdrProfile.Description) { $hdrProfile.Description } else { [IO.Path]::GetFileNameWithoutExtension($name) }
    "$label ($($hdrProfile.PeakNits) nits)"
}

# Returns whether a profile is installed and states its peak brightness.
function Test-ListedProfile([string]$name) {
    $name -and $installed.Contains($name) -and $installed[$name].PeakNits -gt 0
}

$current = $null
$linked = @()
$display = Get-PrimaryDisplay
if ($display) {
    try {
        $scope = [HdrControl]::GetScope($display)
        $linked = @(Get-LinkedHdrProfiles $display $scope)
        $current = [HdrControl]::GetHdrProfile($display, $scope)
    }
    catch {
        Write-Output "Warning: could not read the profiles linked to $($display.FriendlyName): $_"
    }
}

$choices = @()
if (Test-ListedProfile $current) {
    $choices += [ordered]@{ value = $current; label = "$(Get-ProfileLabel $current), current" }
}
$others = foreach ($name in $installed.Keys) {
    if ($name -ne $current -and (Test-ListedProfile $name) -and ($name -notlike 'TPLCD_*' -or $linked -contains $name)) {
        [ordered]@{ value = $name; label = Get-ProfileLabel $name }
    }
}
$choices += @($others | Sort-Object { $_.label })

ConvertTo-Json -InputObject @($choices) -Compress | Set-Content -LiteralPath $env:SUNSHINE_PLUGIN_CHOICES_FILE -Encoding UTF8
Write-Output "Listed $($choices.Count) HDR profiles"
