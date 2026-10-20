@echo off
rem Two launchers and two games on this PC (host + guest) to try co-op alone. See two-launchers.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0two-launchers.ps1" %*
pause
