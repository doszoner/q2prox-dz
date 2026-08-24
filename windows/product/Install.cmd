@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if errorlevel 1 (
  echo Installation failed.
  pause
  exit /b 1
)
start "" "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Q2PRO-X - Dos.Zone.lnk"
