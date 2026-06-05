@echo off
setlocal

set "ROOT=%~dp0"
set "PLATFORM=%WINMERGE_PLATFORM%"
if "%PLATFORM%"=="" set "PLATFORM=x64"

set "EXEDIR=%ROOT%Build\%PLATFORM%\Release"
set "EXE=%EXEDIR%\WinMergeU.exe"
set "WINMERGE_EXCEL_LOG=1"
set "WINMERGE_LOG_FILE=%TEMP%\WinMerge.log"

if not exist "%EXE%" (
  echo Executable not found: "%EXE%"
  echo Build the Release configuration for %PLATFORM% first.
  exit /b 1
)

echo WinMerge log: "%WINMERGE_LOG_FILE%"
if exist "%WINMERGE_LOG_FILE%" del /q "%WINMERGE_LOG_FILE%"
start "" /D "%EXEDIR%" "%EXE%" /s- /cfg "Settings/Logging=1" /new %*
