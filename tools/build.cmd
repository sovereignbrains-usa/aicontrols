@echo off
rem Build helper: the VS dev environment, then a CMake preset (dev, dist, analyze).
rem Usage: tools\build.cmd <preset>
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
set VSLANG=1033
cd /d "%~dp0.."
rem The configure output is only shown when it fails; the log is not left in %TEMP%.
set "LOG=%TEMP%\aic-configure-%1.log"
cmake --preset %1 >"%LOG%" 2>&1
if errorlevel 1 (
    type "%LOG%"
    del "%LOG%" 2>nul
    exit /b 1
)
del "%LOG%" 2>nul
cmake --build --preset %1 2>&1
