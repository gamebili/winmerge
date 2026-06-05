@echo off
setlocal

set "ROOT=%~dp0"
set "PLATFORM=%WINMERGE_PLATFORM%"
if "%PLATFORM%"=="" set "PLATFORM=x64"

set "EXEDIR=%ROOT%Build\%PLATFORM%\Debug"
set "EXE=%EXEDIR%\WinMergeU.exe"

if not exist "%EXE%" (
  echo Executable not found: "%EXE%"
  echo Build the Debug configuration for %PLATFORM% first.
  exit /b 1
)

start "" /D "%EXEDIR%" "%EXE%" /new %*
