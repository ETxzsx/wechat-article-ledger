@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo Starting WeChat Link Watcher ...
echo It records every WeChat article link automatically:
echo   - browser opens an article  -> recorded via history
echo   - copy a link anywhere     -> recorded via clipboard
echo.
cpp\wechat_link_watcher.exe
echo.
echo Watcher stopped. Press any key to close.
pause >nul