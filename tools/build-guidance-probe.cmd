@echo off
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\guidance_probe.cpp /Febuild\guidance_probe.exe /Fobuild\guidance_probe.obj /link user32.lib ole32.lib oleaut32.lib uiautomationcore.lib shell32.lib propsys.lib shcore.lib gdiplus.lib gdi32.lib
if errorlevel 1 exit /b 1
set result=0
popd
exit /b %result%
