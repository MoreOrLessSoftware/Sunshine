# Makes the chosen profile the primary display's HDR profile for the stream.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

Set-StreamHdrProfile
