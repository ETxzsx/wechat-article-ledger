@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
set "PY=%USERPROFILE%\.dsh\dsh-runtimes\dsh-plugin-desktop-primary-runtime\dependencies\python\python.exe"
if exist "%PY%" goto havepy
set "PY=E:\python\python.exe"
if exist "%PY%" goto havepy
set "PY=python"
:havepyecho Starting floating button (backend will be launched automatically) ...
set "PYW=%PY:python.exe=pythonw.exe%"
if not exist "%PYW%" set "PYW=%PY%"
start "" "%PYW%" floater.py
