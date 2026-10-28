@echo off
rem Builds and runs the M3 smoke test (params, profiles, extrude/revolve/booleans, save/load)
rem against the real OpenCASCADE. Console only, no GPU. Needs one main build.bat run first.
setlocal
set "SRC=%~dp0.."
if not defined KESTRAL_BUILD set "KESTRAL_BUILD=C:\KestrelBuild"
set "VCP=%KESTRAL_BUILD%\vcpkg_installed\x64-windows"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :fail

set "OBJS=%KESTRAL_BUILD%\m3test"
if not exist "%OBJS%" mkdir "%OBJS%"
cd /d "%OBJS%"

cl /nologo /std:c++20 /EHa /utf-8 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRC%\src" /I"%VCP%\include" /I"%VCP%\include\opencascade" ^
   "%SRC%\tools\m3_test.cpp" "%SRC%\src\core\Params.cpp" "%SRC%\src\core\Curves.cpp" ^
   "%SRC%\src\sketch\Sketch.cpp" "%SRC%\src\sketch\Profiles.cpp" ^
   "%SRC%\src\core\Document.cpp" "%SRC%\src\core\SaveLoad.cpp" ^
   "%SRC%\src\kernel\Kernel.cpp" "%SRC%\src\kernel\Features.cpp" ^
   "%SRC%\src\core\Log.cpp" "%SRC%\src\platform\Platform.cpp" ^
   /Fe:m3test.exe ^
   /link /LIBPATH:"%VCP%\lib" TKernel.lib TKMath.lib TKPrim.lib TKBRep.lib TKBO.lib TKTopAlgo.lib ^
   TKGeomAlgo.lib TKShHealing.lib TKGeomBase.lib TKG3d.lib TKG2d.lib TKMesh.lib TKDE.lib TKDESTEP.lib ^
   TKDESTL.lib TKXSBase.lib TKCAF.lib TKCDF.lib TKLCAF.lib TKVCAF.lib TKXCAF.lib TKHLR.lib ^
   TKService.lib TKOffset.lib TKFillet.lib dbghelp.lib comdlg32.lib shell32.lib ole32.lib user32.lib
if errorlevel 1 goto :fail
if not exist m3test.exe goto :fail

set "PATH=%VCP%\bin;%PATH%"
m3test.exe
exit /b %errorlevel%

:fail
echo test build failed
exit /b 1
