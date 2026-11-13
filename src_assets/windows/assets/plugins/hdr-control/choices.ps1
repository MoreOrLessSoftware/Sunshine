# Lists the HDR profiles for the profile option: those linked to the primary display first, its current one at the
# top, then the other installed profiles made for HDR. Profiles that laptop makers install for their own panels
# (TPLCD_*) are left out unless the display uses them.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

$installed = Get-InstalledHdrProfiles

# Names a profile by its description and peak brightness, falling back to its file name.
function Get-ProfileLabel([string]$name) {
    $hdrProfile = $installed[$name]
    $label = if ($hdrProfile -and $hdrProfile.Description) { $hdrProfile.Description } else { [IO.Path]::GetFileNameWithoutExtension($name) }
    if ($hdrProfile -and $hdrProfile.PeakNits -gt 0) {
        $label = "$label ($($hdrProfile.PeakNits) nits)"
    }
    $label
}

$choices = @()
$linked = @()
$display = Get-PrimaryDisplay
if ($display) {
    try {
        $scope = [HdrControl]::GetScope($display)
        $linked = @(Get-LinkedHdrProfiles $display $scope)
        $current = [HdrControl]::GetHdrProfile($display, $scope)
        $group = "Linked to $($display.FriendlyName)"
        if ($current) {
            $choices += [ordered]@{ value = $current; label = "$(Get-ProfileLabel $current), current"; group = $group }
        }
        foreach ($name in $linked) {
            if ($name -ne $current) {
                $choices += [ordered]@{ value = $name; label = Get-ProfileLabel $name; group = $group }
            }
        }
        $linked += $current
    }
    catch {
        Write-Output "Warning: could not read the profiles linked to $($display.FriendlyName): $_"
    }
}

foreach ($name in $installed.Keys) {
    if ($linked -notcontains $name -and $name -notlike 'TPLCD_*') {
        $choices += [ordered]@{ value = $name; label = Get-ProfileLabel $name; group = 'Installed' }
    }
}

ConvertTo-Json -InputObject @($choices) -Compress | Set-Content -LiteralPath $env:SUNSHINE_PLUGIN_CHOICES_FILE -Encoding UTF8
Write-Output "Listed $($choices.Count) HDR profiles"
