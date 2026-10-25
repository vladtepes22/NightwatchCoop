@echo off
setlocal
REM Co-op network doctor: drop the session's logs (host + guests) onto this file, or run
REM   net_doctor.bat [--redactnames] [--redactips] LOG [LOG ...]
REM The reply is copied to the clipboard and shown here (scripts\net_doctor.py).
cd /d "%~dp0"
set "PY="
if exist "%~dp0python\python.exe" set "PY=%~dp0python\python.exe"
if "%PY%"=="" for %%P in (py.exe python.exe) do if "%PY%"=="" where %%P >nul 2>nul && set "PY=%%P"
if "%PY%"=="" (
    echo Python not found. Use the release zip, which has Python in its python\ folder.
    pause
    exit /b 1
)
if "%~1"=="" (
    echo Drop the party logs of one session ^(host and guests^) onto net_doctor.bat.
    pause
    exit /b 1
)
set "PYTHONDONTWRITEBYTECODE=1"
set "PYTHONIOENCODING=utf-8"
"%PY%" -I "%~dp0scripts\net_doctor.py" %* > "%TEMP%\net_doctor_reply.txt"
type "%TEMP%\net_doctor_reply.txt"
clip < "%TEMP%\net_doctor_reply.txt"
echo.
echo (copied to the clipboard)
pause
