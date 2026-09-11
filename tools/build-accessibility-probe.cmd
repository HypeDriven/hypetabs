@echo off
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\accessibility_probe.cpp /Febuild\accessibility_probe.exe /Fobuild\accessibility_probe.obj /link user32.lib ole32.lib oleaut32.lib uiautomationcore.lib
if errorlevel 1 exit /b 1
set result=0
popd
exit /b %result%
