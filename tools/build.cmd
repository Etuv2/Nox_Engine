@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build.ps1" %*
exit /b %ERRORLEVEL%
