# Applies the configured V-Sync mode, remembering the previous settings for undo.ps1.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$mode = $env:SUNSHINE_PLUGIN_OPT_MODE

foreach ($backend in Get-VsyncBackends) {
    switch ($backend) {
        'nvidia' {
            $stateFile = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'previous-mode.txt'
            # Keep the mode saved by an earlier stream that did not run undo.ps1.
            if (-not (Test-Path $stateFile)) {
                # Collect all output before checking the exit code: in Windows PowerShell, piping a native
                # command into Select-Object -First stops it early and leaves $LASTEXITCODE at -1.
                $status = @(& $nvidiaExe status)
                if ($LASTEXITCODE -ne 0) {
                    throw "vsynctoggle status failed with exit code $LASTEXITCODE"
                }
                Set-Content -Path $stateFile -Value "$($status | Select-Object -First 1)".Trim()
            }

            Write-Output "NVIDIA: setting V-Sync to $mode (was $(Get-Content $stateFile))"
            & $nvidiaExe $mode
            if ($LASTEXITCODE -ne 0) {
                throw "vsynctoggle $mode failed with exit code $LASTEXITCODE"
            }
        }
        'amd' {
            $stateFile = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'previous-amd.json'
            $previous = @{}
            if (Test-Path $stateFile) {
                # Keep the values saved by an earlier stream that did not run undo.ps1.
                (Get-Content $stateFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object {
                    $previous[$_.Name] = $_.Value
                }
            }

            $digit = $amdModes[$mode]
            foreach ($key in Get-AmdUmdKeys) {
                if (-not $previous.ContainsKey($key)) {
                    # An empty string records that the value did not exist.
                    $current = Get-AmdVsync $key
                    $previous[$key] = if ($null -eq $current) { '' } else { $current }
                }
                Set-AmdVsync $key $digit
                Write-Output "AMD adapter $(Split-Path (Split-Path $key) -Leaf): Wait for Vertical Refresh $($previous[$key]) -> $digit ($mode)"
            }
            $previous | ConvertTo-Json | Set-Content -Path $stateFile -Encoding UTF8
        }
    }
}
