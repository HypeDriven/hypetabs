@echo off
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0test-shortcut.ps1"
exit /b %errorlevel%
