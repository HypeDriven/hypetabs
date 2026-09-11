@echo off
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build mkdir build
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE native\bridge.cpp /Febuild\HypeTabs.Bridge.exe /Fobuild\bridge.obj /link advapi32.lib user32.lib
if errorlevel 1 exit /b 1
node tools\build-extension.mjs
if errorlevel 1 exit /b 1
node tools\embed-assets.mjs
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX native\main.cpp /Febuild\HypeTabs.exe /Fobuild\main.obj /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib gdi32.lib crypt32.lib oleaut32.lib uiautomationcore.lib shcore.lib propsys.lib oleacc.lib dwmapi.lib uxtheme.lib comdlg32.lib
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\core_tests.cpp /Febuild\core_tests.exe /Fobuild\core_tests.obj
if errorlevel 1 exit /b 1
build\core_tests.exe
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\protocol_tests.cpp /Febuild\protocol_tests.exe /Fobuild\protocol_tests.obj
if errorlevel 1 exit /b 1
build\protocol_tests.exe
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\browser_state_tests.cpp /Febuild\browser_state_tests.exe /Fobuild\browser_state_tests.obj /link ole32.lib
if errorlevel 1 exit /b 1
build\browser_state_tests.exe
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\history_tests.cpp /Febuild\history_tests.exe /Fobuild\history_tests.obj /link ole32.lib advapi32.lib crypt32.lib user32.lib
if errorlevel 1 exit /b 1
build\history_tests.exe
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT tests\startup_tests.cpp /Febuild\startup_tests.exe /Fobuild\startup_tests.obj /link ole32.lib advapi32.lib
if errorlevel 1 exit /b 1
build\startup_tests.exe
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX tests\profile_setup_tests.cpp /Febuild\profile_setup_tests.exe /Fobuild\profile_setup_tests.obj /link ole32.lib shell32.lib advapi32.lib
if errorlevel 1 exit /b 1
build\profile_setup_tests.exe
set result=%errorlevel%
popd
exit /b %result%
