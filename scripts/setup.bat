@echo off
setlocal

set VERSION=%~1
if "%VERSION%"=="" set VERSION=146.0.3856.97

set DEST=%~2
if "%DEST%"=="" set DEST=%~dp0..\webview2_runtime\%VERSION%

echo [setup] WebView2 runtime %VERSION% → %DEST%

powershell -NoProfile -ExecutionPolicy Bypass ^
    -File "%~dp0setup.ps1" ^
    -Version "%VERSION%" ^
    -Destination "%DEST%"

exit /b %ERRORLEVEL%