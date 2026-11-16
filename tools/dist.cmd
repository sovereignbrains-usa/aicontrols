@echo off
rem Release build and installer.
rem Usage: tools\dist.cmd <version>      e.g. tools\dist.cmd 0.1.0
rem Makes build\installer\AIControls-Setup-<version>.exe (per user, no administrator rights).
if "%~1"=="" (
  echo usage: tools\dist.cmd ^<version^>
  exit /b 2
)
setlocal
cd /d "%~dp0.."
call "%~dp0build.cmd" dist || exit /b 1
if not exist build\dist\aicontrols.exe (
  echo build\dist\aicontrols.exe is missing
  exit /b 1
)
rem Per-user installs land in LocalAppData. Program Files remains the first guess.
set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo Inno Setup 6 was not found
  exit /b 1
)
"%ISCC%" /Qp /DAppVersion=%~1 /DSourceDir="%CD%\build\dist" /DOutputDir="%CD%\build\installer" installer\aicontrols.iss || exit /b 1
echo.
echo build\installer\AIControls-Setup-%~1.exe
