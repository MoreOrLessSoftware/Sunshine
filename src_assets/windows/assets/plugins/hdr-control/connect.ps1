# A client resuming the application may have its own HDR mode and profile, so set them again. The previous settings
# are only put back when the application quits.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

Set-StreamHdr
