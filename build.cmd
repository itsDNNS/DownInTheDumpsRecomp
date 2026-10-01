@echo off
rem Builds the ready-to-play folder "Down in the Dumps" from this source code and your original CDs.
rem The build tools are downloaded once into .tools (see tools\build.ps1 and README.md).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build.ps1" %*
echo.
pause
