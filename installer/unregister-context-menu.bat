@echo off
setlocal
pushd "%~dp0"
if not exist "volchay-register.exe" (
    echo volchay-register.exe not found next to this script.
    popd
    exit /b 1
)
volchay-register.exe unregister
set RC=%ERRORLEVEL%
popd
endlocal & exit /b %RC%
