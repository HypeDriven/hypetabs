@echo off
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0probe-chrome.ps1"
exit /b %errorlevel%
