@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-install.ps1"
exit /b %errorlevel%
