@echo off
setlocal
if defined VSCMD_VER goto compile
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%V"
if not defined VSROOT exit /b 1
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
:compile
pushd "%~dp0"
if not exist bin mkdir bin
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE /DNDEBUG /Fo"bin\\" /Fe:bin\ExplorerRecall.exe main.cpp shell.cpp store.cpp taskbar.cpp preferences.cpp /link /MANIFEST:EMBED /SUBSYSTEM:WINDOWS comctl32.lib shlwapi.lib shell32.lib ole32.lib oleaut32.lib uuid.lib user32.lib gdi32.lib advapi32.lib winsqlite3.lib uiautomationcore.lib dwmapi.lib uxtheme.lib propsys.lib
set "BUILD_RESULT=%ERRORLEVEL%"
popd
exit /b %BUILD_RESULT%
