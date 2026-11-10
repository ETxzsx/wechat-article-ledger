@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /MT kernel.cpp /Fe:kernel.exe /link user32.lib gdi32.lib shell32.lib ole32.lib ws2_32.lib winhttp.lib comctl32.lib /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
if errorlevel 1 (echo BUILD_FAILED & exit /b 1)
echo BUILD_OK