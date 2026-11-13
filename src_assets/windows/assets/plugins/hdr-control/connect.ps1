# A client resuming the application may have chosen its own profile, so set it again. The previous profile is only
# put back when the application quits.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

Set-StreamHdrProfile
