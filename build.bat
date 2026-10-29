@echo off
setlocal EnableExtensions
rem ===================================================================
rem  Kestral build script
rem  First run: downloads vcpkg and builds dependencies (OpenCASCADE is
rem  the big one, roughly 20 to 40 minutes). Later builds take seconds.
rem  Dependencies and build files live in C:\KestrelBuild because the
rem  project path contains "&" and spaces, which some build tools hate.
rem  Set KESTRAL_BUILD to use a different folder.
rem ===================================================================
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
if not defined KESTRAL_BUILD set "KESTRAL_BUILD=C:\KestrelBuild"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :novs
set "VCPKG_ROOT=%KESTRAL_BUILD%\vcpkg"
set "VCPKG_DISABLE_METRICS=1"

where git >nul 2>nul
if errorlevel 1 (
  echo [Kestral] Git was not found. Install Git for Windows from https://git-scm.com/download/win
  goto :end
)

if not exist "%KESTRAL_BUILD%" mkdir "%KESTRAL_BUILD%"
if not exist "%VCPKG_ROOT%\.git" (
  echo [Kestral] Downloading vcpkg...
  git clone https://github.com/microsoft/vcpkg "%VCPKG_ROOT%"
  if errorlevel 1 goto :fail
)
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
  if errorlevel 1 goto :fail
)
git -C "%VCPKG_ROOT%" fetch -q origin >nul 2>nul

echo [Kestral] Configuring (first time builds dependencies, be patient)...
cmake -S "%SRC%" -B "%KESTRAL_BUILD%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DVCPKG_INSTALLED_DIR="%KESTRAL_BUILD%\vcpkg_installed"
if errorlevel 1 goto :fail

echo [Kestral] Building...
cmake --build "%KESTRAL_BUILD%\build"
if errorlevel 1 goto :fail

if not exist "%SRC%\bin" mkdir "%SRC%\bin"
rem A running Kestral (or the MCP bridge an AI app started) keeps bin\Kestral.exe locked. Windows
rem lets a running program be renamed, so move it aside and copy the new one in its place.
if exist "%SRC%\bin\Kestral.old.exe" del /q "%SRC%\bin\Kestral.old.exe" >nul 2>nul
if exist "%SRC%\bin\Kestral.exe" move /y "%SRC%\bin\Kestral.exe" "%SRC%\bin\Kestral.old.exe" >nul 2>nul
robocopy "%KESTRAL_BUILD%\build" "%SRC%\bin" *.exe *.dll /R:2 /W:1 /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
  echo [Kestral] Could not copy into bin. Close Kestral and any AI app using it, then run build.bat again.
  goto :fail
)
echo [Kestral] Restart Kestral to use the new version.
echo.
echo [Kestral] Done. Run: run.bat   (or bin\Kestral.exe)
goto :end

:novs
echo [Kestral] Visual Studio 2022 C++ tools were not found.
echo           Install "Build Tools for Visual Studio 2022" and tick "Desktop development with C++".
goto :end

:fail
echo.
echo [Kestral] Build failed. Scroll up for the first error, or send it to Claude.
goto :end

:end
echo.
pause
exit /b 0
