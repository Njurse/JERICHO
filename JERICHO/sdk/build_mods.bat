@echo off
rem ============================================================
rem  JERICHO addon SDK - build_mods.bat
rem
rem  Compiles an addon folder (mod.toml + source) into a loadable
rem  DLL that the game picks up at runtime - the game exe is never
rem  rebuilt and no game source is needed.
rem
rem  Usage:  build_mods.bat <mod-folder>
rem    e.g.  build_mods.bat example
rem
rem  Requires: Visual Studio 2019/2022 (C++ toolset) on Windows.
rem  Produces: <mod-folder>\<id>.dll   (id = the folder name)
rem  Install:  copy the DLL into the game's JERICHO\MODS\<id>\
rem            folder and toggle the addon in Options -> JERICHO.
rem ============================================================
setlocal EnableExtensions EnableDelayedExpansion

set "MOD_FOLDER=%~f1"

if "%MOD_FOLDER%"=="" goto usage
if not exist "%MOD_FOLDER%\mod.toml" goto usage

for %%F in ("%MOD_FOLDER%") do set "MOD_ID=%%~nxF"

rem run from the SDK dir so %~dp0 below is always an absolute path
pushd "%~dp0"
set "SDK_DIR=%CD%\"

rem ---- locate the MSVC toolchain via vswhere ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novswhere

set "VSINSTALL="
for /f "usebackq delims=" %%v in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%v"

if "%VSINSTALL%"=="" goto novswhere

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 goto novswhere

rem ---- compile: one DLL per addon, linking the game's export surface ----
if not exist "%TEMP%\jericho_build" mkdir "%TEMP%\jericho_build"

set "RSP=%TEMP%\jericho_build\sources.rsp"
if exist "%RSP%" del "%RSP%"
for %%f in ("%MOD_FOLDER%\*.c") do echo "%%f" >> "%RSP%"

if not exist "%RSP%" (
    echo JERICHO: no .c source files in %MOD_FOLDER%
    exit /b 2
)

cl /nologo /LD /TP /EHsc /DNDEBUG /DJERICHO_MODULE_BUILD ^
   "/I%SDK_DIR%include" "/I%MOD_FOLDER%" ^
   /Fo%TEMP%\jericho_build\ ^
   @%RSP% ^
   /link "/LIBPATH:%SDK_DIR%lib\x64\Release" REDRIVER2.lib ^
   "/OUT:%MOD_FOLDER%\%MOD_ID%.dll"

if errorlevel 1 (
    echo.
    echo JERICHO: build failed - see the compiler errors above.
    exit /b 1
)

echo.
echo JERICHO: built %MOD_ID%.dll
echo          Copy it into the game's JERICHO\MODS\%MOD_ID%\ folder,
echo          then enable "%MOD_ID%" in Options -^> JERICHO.
exit /b 0

:usage
echo usage: build_mods.bat ^<mod-folder^>   ^(a folder with mod.toml + source^)
echo   e.g.  build_mods.bat example
exit /b 2

:novswhere
echo JERICHO: Visual Studio with the C++ toolset was not found.
echo Install "Desktop development with C++" and try again.
exit /b 3
