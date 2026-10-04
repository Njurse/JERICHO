@echo off
rem ============================================================
rem  debugorbit - build this addon and install it into the runtime
rem  MODS folder the launcher's exe reads.
rem
rem  Usage:  build.bat        (run it from anywhere)
rem
rem  Why a helper: the mods-only premake solution only declares a
rem  Release config and links bin/Release/JERICHO.lib, which this
rem  tree does not have, while the running exe is Release_dev. The
rem  SDK build is self-contained (cl.exe + the SDK import lib), and
rem  this script then mirrors the result where the loader looks.
rem ============================================================
setlocal EnableExtensions

set "MODDIR=%~dp0"
set "REPO=%MODDIR%..\..\.."
set "ID=debugorbit"
set "RUNTIME=%REPO%\src_rebuild\bin\Release_dev\JERICHO\MODS\%ID%"

call "%REPO%\JERICHO\sdk\build_mods.bat" "%MODDIR:~0,-1%"
if errorlevel 1 (
	echo debugorbit: build failed
	exit /b 1
)

rem mirror what the loader needs (mod.toml + <id>.dll); the pdb rides along
rem so a crash dump can be resolved against this build
if not exist "%RUNTIME%" mkdir "%RUNTIME%" >nul 2>&1
copy /y "%MODDIR%%ID%.dll" "%RUNTIME%\" >nul || exit /b 1
copy /y "%MODDIR%mod.toml" "%RUNTIME%\" >nul || exit /b 1
if exist "%MODDIR%%ID%.pdb" copy /y "%MODDIR%%ID%.pdb" "%RUNTIME%\" >nul

echo debugorbit: installed into src_rebuild\bin\Release_dev\JERICHO\MODS\%ID%
echo             (enable it in that JERICHO\CONFIG\modlist.ini if it is not listed)
exit /b 0
