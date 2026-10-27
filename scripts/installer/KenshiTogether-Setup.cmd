@echo off
rem Double-click entry point for the Kenshi Together installer. Arguments pass through
rem (e.g. KenshiTogether-Setup.cmd -Uninstall). See KenshiTogether-Setup.ps1.
setlocal
cd /d "%~dp0"
title Kenshi Together setup
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0KenshiTogether-Setup.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" echo Setup did not finish (exit %RC%). The message above says why.
pause
exit /b %RC%
