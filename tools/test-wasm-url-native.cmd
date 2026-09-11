@echo off
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build mkdir build
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\wasm_url_tests.cpp extension\wasm\url.cpp /Febuild\wasm_url_tests.exe /Fobuild\
if errorlevel 1 exit /b 1
build\wasm_url_tests.exe
set result=%errorlevel%
popd
exit /b %result%
