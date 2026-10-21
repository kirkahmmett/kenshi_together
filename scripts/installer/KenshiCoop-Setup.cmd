@echo off
rem Double-click entry point for the KenshiCoop installer. Arguments pass through
rem (e.g. KenshiCoop-Setup.cmd -Uninstall). See KenshiCoop-Setup.ps1.
setlocal
cd /d "%~dp0"
title KenshiCoop setup
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0KenshiCoop-Setup.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" echo Setup did not finish (exit %RC%). The message above says why.
pause
exit /b %RC%
