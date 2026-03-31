@echo off
REM Creates a Windows Scheduled Task that runs upload_to_r2.py every hour.
REM Run this script as Administrator.
REM
REM Edit the variables below before running:

SET PYTHON_EXE=python
SET SCRIPT_PATH=%~dp0upload_to_r2.py
SET TASK_NAME=GWObserver-R2-Upload

echo Creating scheduled task: %TASK_NAME%
echo Script: %SCRIPT_PATH%
echo Schedule: every 1 hour
echo.

schtasks /create /tn "%TASK_NAME%" /tr "\"%PYTHON_EXE%\" \"%SCRIPT_PATH%\"" /sc hourly /mo 1 /f

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Task created successfully.
    echo To run it now:  schtasks /run /tn "%TASK_NAME%"
    echo To delete it:   schtasks /delete /tn "%TASK_NAME%" /f
    echo To view status: schtasks /query /tn "%TASK_NAME%"
) else (
    echo.
    echo Failed to create task. Make sure you are running as Administrator.
)

pause
