@echo off
cd /d "%~dp0"

where node >nul 2>&1
if errorlevel 1 goto noNode

REM If port 8777 is already held, the service itself steps to the next free port
REM (web/server.js bindServer) and prints the actual address - we never kill an old instance.

echo.
echo ============================================================
echo  DLSS5NR service starting... browser will open automatically.
echo  This window is the service terminal.
echo  Closing this window stops the service and cleans temp files.
echo ============================================================
echo.

REM server_guard.exe hosts node web\server.js --open and cleans the disposable temp
REM dirs on console close. In a plain source checkout the compiled guard is absent,
REM so fall back to launching node directly (dev mode, no close-time cleanup).
if exist "%~dp0server_guard.exe" (
    "%~dp0server_guard.exe"
) else (
    echo  [dev mode] server_guard.exe not found - starting node directly.
    echo  Closing this window stops the service, but temp files are only cleaned
    echo  by the server itself or the next launch.
    node "%~dp0web\server.js" --open
)

echo.
echo Service stopped. Press any key to close this window.
pause >nul
exit /b 0

:noNode
echo.
echo [ERROR] node.exe not found in PATH.
echo Install Node.js and add it to PATH, then try again.
echo.
pause >nul
exit /b 1