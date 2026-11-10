@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
if not exist "java\jre\bin\javaw.exe" (
  echo [ERROR] java\jre not found.
  pause
  exit /b 1
)
set "APPDIR=%~dp0"
if "%APPDIR:~-1%"=="\" set "APPDIR=%APPDIR:~0,-1%"
start "" "java\jre\bin\javaw.exe" --module-path "java\javafx\lib" --add-modules javafx.controls,javafx.web -Dfile.encoding=UTF-8 "-Dapp.dir=%APPDIR%" -cp "java\classes" DesktopApp