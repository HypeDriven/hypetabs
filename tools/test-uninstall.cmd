@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-uninstall.ps1"
exit /b %errorlevel%
