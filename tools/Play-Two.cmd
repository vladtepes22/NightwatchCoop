@echo off
rem Two copies of the game (host + guest) in small windows, to try co-op alone. See play-two.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0play-two.ps1" %*
pause
