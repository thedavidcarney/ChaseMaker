@echo off
REM cm_build.bat — invoke vcvars64 then cmake build for ChaseMaker.
REM Usage: cm_build.bat
REM        cm_build.bat configure   (re-runs cmake configure)
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if "%~1"=="configure" (
  cmake --preset win-x64-release || exit /b 1
)
cmake --build --preset win-x64-release
exit /b %ERRORLEVEL%
