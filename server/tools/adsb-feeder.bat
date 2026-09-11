@echo off
REM Windows launcher for the ADS-B feeder.
REM
REM 1. Install Python 3 from python.org (tick "Add python.exe to PATH").
REM 2. Put this file and adsb_feeder.py in the same folder.
REM 3. Edit the two lines below with the values from your dashboard.
REM 4. Double-click to test. Leave the window open - closing it stops the feed.
REM
REM To run it unattended, so it survives logoff and starts at boot, use Task
REM Scheduler: Create Task, "Run whether user is logged on or not", trigger
REM "At startup", action "Start a program" pointing at this .bat.

set ADSB_SERVER=changeme.example.com
set ADSB_PORT=0
set ADSB_RECEIVER=127.0.0.1
set ADSB_RECEIVER_PORT=30003

if "%ADSB_PORT%"=="0" (
  echo Edit this file first: set ADSB_SERVER and ADSB_PORT to the values
  echo shown on your account dashboard for this device.
  pause
  exit /b 1
)

REM "python" not "python3": on Windows python3 hits the Microsoft Store
REM alias stub rather than a real interpreter.
python "%~dp0adsb_feeder.py" --server %ADSB_SERVER% --port %ADSB_PORT% ^
  --receiver %ADSB_RECEIVER% --receiver-port %ADSB_RECEIVER_PORT%
pause
