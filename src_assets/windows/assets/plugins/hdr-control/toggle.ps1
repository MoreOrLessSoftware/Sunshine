# Client command: turns HDR off for the primary display when it is on, or on when it is off, making the chosen profile
# its HDR profile when HDR comes on.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

Switch-StreamHdr
