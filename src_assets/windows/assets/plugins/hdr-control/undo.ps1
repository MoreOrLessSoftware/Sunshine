# Puts back the HDR modes and profiles the stream changed.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hdr.ps1')

Restore-StreamHdr
