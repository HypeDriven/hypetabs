@echo off
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0smoke.ps1"
exit /b %errorlevel%
