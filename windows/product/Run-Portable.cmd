@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0q2dz-launch.ps1"
set "Q2DZ_EXIT=%ERRORLEVEL%"
if "%Q2DZ_EXIT%"=="0" exit /b 0
if "%Q2DZ_EXIT%"=="10" exit /b 0
echo Q2PRO-X - Dos.Zone ended with error %Q2DZ_EXIT%.
pause
exit /b %Q2DZ_EXIT%
