# Restores the V-Sync settings saved by do.ps1.

. (Join-Path $PSScriptRoot 'common.ps1')

$failed = $false

foreach ($backend in Get-VsyncBackends) {
    switch ($backend) {
        'nvidia' {
            $stateFile = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'previous-mode.txt'
            if (-not (Test-Path $stateFile)) {
                Write-Output 'NVIDIA: no saved V-Sync mode to restore'
                continue
            }

            $previous = (Get-Content $stateFile -Raw).Trim()
            $known = @('default', 'off', 'on', 'app', 'fast', 'half', 'third', 'quarter')
            if ($known -notcontains $previous) {
                Write-Output "NVIDIA: unrecognized saved V-Sync mode '$previous', restoring 'default'"
                $previous = 'default'
            }

            Write-Output "NVIDIA: restoring V-Sync to $previous"
            & $nvidiaExe $previous
            if ($LASTEXITCODE -eq 0) {
                Remove-Item $stateFile -Force
            }
            else {
                $failed = $true
            }
        }
        'amd' {
            $stateFile = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'previous-amd.json'
            if (-not (Test-Path $stateFile)) {
                Write-Output 'AMD: no saved V-Sync settings to restore'
                continue
            }

            try {
                (Get-Content $stateFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object {
                    $key = $_.Name
                    if ($_.Value -eq '') {
                        Remove-ItemProperty -Path $key -Name VSyncControl -ErrorAction SilentlyContinue
                        Write-Output "AMD adapter $(Split-Path (Split-Path $key) -Leaf): Wait for Vertical Refresh reset to the driver default"
                    }
                    else {
                        Set-AmdVsync $key $_.Value
                        Write-Output "AMD adapter $(Split-Path (Split-Path $key) -Leaf): Wait for Vertical Refresh restored to $($_.Value)"
                    }
                }
                Remove-Item $stateFile -Force
            }
            catch {
                Write-Output "AMD: failed to restore V-Sync settings: $_"
                $failed = $true
            }
        }
    }
}

if ($failed) {
    exit 1
}
