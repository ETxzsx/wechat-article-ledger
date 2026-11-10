@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
if not exist "java\jre\bin\java.exe" (
  echo [ERROR] java\jre not found.
  pause
  exit /b 1
)
set "APPDIR=%~dp0"
if "%APPDIR:~-1%"=="\" set "APPDIR=%APPDIR:~0,-1%"
"java\jre\bin\java.exe" -Dfile.encoding=UTF-8 "-Dapp.dir=%APPDIR%" -cp "java\classes" LedgerFrontend
echo.
echo Frontend stopped. Press any key to close.
pause >nul