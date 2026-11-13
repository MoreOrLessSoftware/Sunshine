# Puts back the HDR profiles the stream replaced.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

Restore-HdrProfiles
