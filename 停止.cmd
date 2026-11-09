@echo off
chcp 65001 >nul
setlocal
echo Stopping floating button and server ...
powershell -NoProfile -Command "try{$r=Invoke-RestMethod -Method Post -Uri http://127.0.0.1:8770/api/floating/stop -ContentType 'application/json' -Body '{}'}catch{}; Get-NetTCPConnection -LocalPort 8770 -State Listen -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }; 'done'"
