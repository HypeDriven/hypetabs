@echo off
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\taskbar_probe.cpp /Febuild\taskbar_probe.exe /Fobuild\taskbar_probe.obj /link user32.lib ole32.lib oleaut32.lib uiautomationcore.lib shell32.lib propsys.lib
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\probe-chrome.ps1 -Taskbar
set result=%errorlevel%
popd
exit /b %result%
