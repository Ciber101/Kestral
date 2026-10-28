@echo off
if not exist "%~dp0bin\Kestral.exe" (
  echo Kestral is not built yet. Run build.bat first.
  pause
  exit /b 1
)
start "" "%~dp0bin\Kestral.exe" %*
