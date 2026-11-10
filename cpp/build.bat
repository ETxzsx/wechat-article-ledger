@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 "wechat_link_watcher.cpp" /Fe:"wechat_link_watcher.exe" /link user32.lib shell32.lib
if errorlevel 1 (echo BUILD_FAILED & exit /b 1)
echo BUILD_OK