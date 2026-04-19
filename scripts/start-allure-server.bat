@echo off
title Allure Report Server (port 4040)
echo.
echo ========================================
echo   Allure Report Server
echo ========================================
echo.
echo Anyone on your LAN can open:
powershell -NoProfile -Command "Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notmatch '^(127\.|169\.254\.)' -and ($_.PrefixOrigin -eq 'Dhcp' -or $_.PrefixOrigin -eq 'Manual') } | ForEach-Object { Write-Host ('   http://' + $_.IPAddress + ':4040/') -ForegroundColor Cyan }"
echo   (or http://localhost:4040/ on this machine)
echo.
echo Close this window to stop the server.
echo ========================================
echo.
"C:\Users\test\.bun\bin\bun.exe" "C:\GhostCOM-src\scripts\allure-server.mjs"
echo.
echo Server stopped.
pause
