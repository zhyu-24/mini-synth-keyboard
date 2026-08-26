@echo off
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"

set "GUI_SCRIPT=%~dp0music_library_gui.py"
set "PYTHON_EXE=%LocalAppData%\Programs\Python\Python313\python.exe"

rem Prefer the verified per-user Python install. This does not depend on PATH.
if exist "%PYTHON_EXE%" (
  "%PYTHON_EXE%" "%GUI_SCRIPT%"
) else (
  where py >nul 2>nul
  if errorlevel 1 (
    echo Python 3 was not found.
    echo Install Python 3.10 or later, then try again.
    pause
    exit /b 1
  )
  py -3 "%GUI_SCRIPT%"
)

set "EXIT_CODE=%ERRORLEVEL%"
if "%EXIT_CODE%"=="0" exit /b 0

echo.
echo The music library manager failed to start. See the error above.
echo If pyserial is missing, run: py -3 -m pip install pyserial
echo If numpy is missing, run: py -3 -m pip install numpy
pause
exit /b %EXIT_CODE%
