# Turns HDR on or off for the stream as chosen, then makes the chosen profile the primary display's HDR profile.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

Set-StreamHdr
