@echo off
rem Double click target for install.ps1, so installing does not mean knowing how
rem to get past the PowerShell execution policy. -ExecutionPolicy Bypass applies
rem to this one process and changes nothing about the machine.
rem
rem Arguments pass straight through:
rem   Install.cmd -DesktopShortcut
rem   Install.cmd -Uninstall
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set RESULT=%ERRORLEVEL%
echo.
pause
endlocal & exit /b %RESULT%
