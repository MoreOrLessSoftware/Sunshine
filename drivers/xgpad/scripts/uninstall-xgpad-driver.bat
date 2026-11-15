@echo off
rem Removes the Sunshine xgpad driver when Sunshine is uninstalled, whether or not the xgpad plugin installed it.
rem Run elevated.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\assets\plugins\xgpad\xgpad-driver.ps1" -Action uninstall -LogFile "%~dp0..\config\xgpad-driver.log"
exit /b %ERRORLEVEL%
