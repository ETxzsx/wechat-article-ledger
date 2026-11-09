@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
set "PY=%USERPROFILE%\.dsh\dsh-runtimes\dsh-plugin-desktop-primary-runtime\dependencies\python\python.exe"
if exist "%PY%" goto havepy
set "PY=E:\python\python.exe"
if exist "%PY%" goto havepy
set "PY=python"
:havepyecho Starting WeChat Ledger app ...
"%PY%" -u server.py
echo.
echo Server stopped. Press any key to close.
pause >nul
