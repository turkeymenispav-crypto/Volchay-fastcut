@echo off
REM Per-user registration: adds "Open with Volchay-fastcut" entry to the
REM Windows context menu for common video file types. No admin required.

setlocal
pushd "%~dp0"
if not exist "volchay-register.exe" (
    echo volchay-register.exe not found next to this script.
    echo Make sure register-context-menu.bat lives in the same folder as
    echo volchay-fastcut.exe and volchay-register.exe.
    popd
    exit /b 1
)
volchay-register.exe register
set RC=%ERRORLEVEL%
popd
echo.
echo (Tip: in Windows 11 the entry appears under "Show more options".)
endlocal & exit /b %RC%
