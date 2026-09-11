@echo off
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\locator_probe.cpp /Febuild\locator_probe.exe /Fobuild\locator_probe.obj /link ole32.lib oleaut32.lib user32.lib uiautomationcore.lib
set result=%errorlevel%
popd
exit /b %result%
