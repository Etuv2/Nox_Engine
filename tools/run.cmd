@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run.ps1" %*
exit /b %ERRORLEVEL%
