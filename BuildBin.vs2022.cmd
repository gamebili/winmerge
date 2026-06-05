cd /d "%~dp0"

call SetVersion.cmd
cscript /nologo //E:jscript ExpandEnvironmenStrings.js Version.in > Version.h

setlocal
for /f "usebackq tokens=*" %%i in (`"%programfiles(x86)%\microsoft visual studio\installer\vswhere.exe" -version [17.0^,18.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set InstallDir=%%i
)
if exist "%InstallDir%\Common7\Tools\vsdevcmd.bat" (
  call "%InstallDir%\Common7\Tools\vsdevcmd.bat
)

if "%1" == "" (
  call :BuildBin ARM64 || goto :eof
  call :BuildBin x86|| goto :eof
  call :BuildBin x64 || goto :eof
) else (
  call :BuildBin %1 || goto :eof
)

goto :eof

:BuildBin
MSBuild WinMerge.sln /t:Rebuild /p:Configuration="Release" /p:Platform="%1" || goto :eof
endlocal

if exist "%SIGNBAT_PATH%" (
  call "%SIGNBAT_PATH%" Build\%1\Release\WinMergeU.exe
)

call :BuildExcel2Tsv %1 || goto :eof
call :CopyPlugins %1 || goto :eof

mkdir Build\%1\Release\%APPVER% 2> NUL
copy Build\%1\Release\*.pdb "Build\%1\Release\%APPVER%\"
goto :eof

:BuildExcel2Tsv
set RUST_TARGET=
if /I "%1" == "x64" set RUST_TARGET=x86_64-pc-windows-msvc
if /I "%1" == "x86" set RUST_TARGET=i686-pc-windows-msvc
if /I "%1" == "ARM64" set RUST_TARGET=aarch64-pc-windows-msvc
if "%RUST_TARGET%" == "" (
  echo Skipping calamine Excel plugin for unsupported platform %1
  goto :eof
)
where cargo.exe > NUL 2> NUL || (
  echo cargo.exe is required to build the calamine Excel plugin.
  exit /b 1
)
rustup target list --installed | findstr /I /C:"%RUST_TARGET%" > NUL || rustup target add %RUST_TARGET% || goto :eof
cargo build --manifest-path "Plugins\Commands\excel2tsv\Cargo.toml" --release --target %RUST_TARGET% --target-dir "Build\Cargo\excel2tsv" || goto :eof
mkdir Build\%1\Release\Commands\excel2tsv 2> NUL
copy /Y "Build\Cargo\excel2tsv\%RUST_TARGET%\release\excel2tsv.exe" "Build\%1\Release\Commands\excel2tsv\" || goto :eof
goto :eof

:CopyPlugins
mkdir Build\%1\Release\MergePlugins 2> NUL
copy /Y "Plugins\Plugins.xml" "Build\%1\Release\MergePlugins\" || goto :eof
copy /Y "Plugins\dlls\*.sct" "Build\%1\Release\MergePlugins\" || goto :eof
copy /Y "Plugins\dlls\%1\*.dll" "Build\%1\Release\MergePlugins\" || goto :eof
goto :eof
