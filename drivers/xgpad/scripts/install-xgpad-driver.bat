@echo off
rem Installs the Sunshine xgpad driver from the xgpad plugin's driver folder. The Sunshine installer runs it when the
rem xgpad plugin is installed, since upgrading runs the old version's uninstall, which removes the driver.
rem Run elevated.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\assets\plugins\xgpad\xgpad-driver.ps1" -Action install -LogFile "%~dp0..\config\xgpad-driver.log"
exit /b %ERRORLEVEL%
