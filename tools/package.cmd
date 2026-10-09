@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Package.ps1" %*
exit /b %ERRORLEVEL%
