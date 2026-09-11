@echo off
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build mkdir build
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\index_benchmark.cpp /Febuild\index_benchmark.exe /Fobuild\index_benchmark.obj
if errorlevel 1 exit /b 1
build\index_benchmark.exe
set result=%errorlevel%
popd
exit /b %result%
