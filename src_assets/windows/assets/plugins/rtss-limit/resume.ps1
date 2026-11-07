# Applies the resuming client's frame rate when "Re-apply the limit when a client resumes" is on.
# The limits saved by do.ps1 when the stream started are kept, so undo.ps1 still restores the originals.

$ErrorActionPreference = 'Stop'

if ($env:SUNSHINE_PLUGIN_OPT_REAPPLY_ON_RESUME -ne 'true') {
    Write-Output 'Re-applying the limit on resume is off'
    exit 0
}

& (Join-Path $PSScriptRoot 'do.ps1')
