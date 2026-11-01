@echo off
rem Installs the Sunshine xgpad driver (optional installer component). Run elevated.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0xgpad-driver.ps1" -Action install
exit /b %ERRORLEVEL%
