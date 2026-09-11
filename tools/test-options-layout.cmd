@echo off
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\options_layout_tests.cpp /Febuild\options_layout_tests.exe /Fobuild\options_layout_tests.obj /link user32.lib gdi32.lib
if errorlevel 1 exit /b 1
build\options_layout_tests.exe
set result=%errorlevel%
popd
exit /b %result%
