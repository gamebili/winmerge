@rem Clear NoDefaultCurrentDirectoryInExePath so the bare-name "call" lines below
@rem (and the child build scripts, which inherit it) resolve from this directory.
set "NoDefaultCurrentDirectoryInExePath="
pushd "%~dp0"
call BuildManual.cmd || goto :eof
call BuildBin.vs2022.cmd %1 %2 || goto :eof
pushd Testing\GoogleTest\UnitTests
UnitTests.exe || goto :eof
popd
call BuildTranslations.cmd
call BuildInstaller.cmd %1 %2
call BuildArc.cmd %1 %2
popd
