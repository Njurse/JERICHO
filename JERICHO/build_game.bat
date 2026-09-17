@echo off
rem ============================================================
rem  JERICHO - build_game.bat
rem
rem  Rebuilds the game with the DEEP modules - the ones whose
rem  mod.toml does NOT declare runtime = "dll". Deep modules are
rem  compiled INTO the exe (premake emits a mod_<id> static lib and
rem  the game links it), so the exe has to be relinked. That is why
rem  JERICHO asks for a restart: the running exe cannot be replaced
rem  while it is running.
rem
rem  Run ONE step per invocation, because the runtime
rem  (src_rebuild/Game/C/JERICHO/src/jer_compile.c) shows a
rem  progress screen ("<name> [i/n]") and has to know which module
rem  is building. It calls this script once per step.
rem
rem  Usage:
rem    build_game.bat <src_rebuild_dir> <configuration> premake
rem    build_game.bat <src_rebuild_dir> <configuration> mod <id>
rem    build_game.bat <src_rebuild_dir> <configuration> exe
rem
rem  Exits 0 on success, non-zero on failure (MSBuild's code).
rem  The game exe is NOT renamed here: the runtime does that around
rem  the "exe" step (Windows locks a running image).
rem ============================================================
setlocal EnableExtensions

set "SRC=%~1"
set "CONF=%~2"
set "STEP=%~3"
set "MODID=%~4"

if not defined SRC (
    echo JERICHO: build_game.bat needs the src_rebuild directory.
    exit /b 2
)
if not defined CONF set "CONF=Release"
if not exist "%SRC%\premake5.lua" (
    echo JERICHO: %SRC% does not look like the src_rebuild tree.
    exit /b 2
)

rem ---- locate MSBuild via vswhere (same as build_mods.bat) ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%v in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%v"
)
if not defined MSBUILD (
    where msbuild >nul 2>nul && set "MSBUILD=msbuild"
)
if not defined MSBUILD (
    echo JERICHO: MSBuild was not found - install "Desktop development with C++".
    exit /b 3
)

pushd "%SRC%"

if /I "%STEP%"=="premake" goto :premake
if /I "%STEP%"=="mod"     goto :mod
if /I "%STEP%"=="exe"     goto :exe

echo JERICHO: unknown build step "%STEP%"
popd
exit /b 2

:premake
rem premake reads the dependency dirs from the environment (as the
rem gen_vc2019*.bat scripts do) - set them relative to this tree.
set "SDL2_DIR=%SRC%\dependencies\SDL2-2.30.2"
set "OPENAL_DIR=%SRC%\dependencies\openal-soft-1.23.1-bin"
set "JPEG_DIR=%SRC%\dependencies\jpeg-9d"
premake5.exe vs2019
set "ERR=%errorlevel%"
goto :done

:mod
if not defined MODID (
    echo JERICHO: the "mod" step needs a module id.
    popd
    exit /b 2
)
echo JERICHO: building deep module %MODID%
"%MSBUILD%" build\mod_%MODID%.vcxproj /p:Configuration=%CONF% /p:Platform=x64 /m /v:m /nologo
set "ERR=%errorlevel%"
goto :done

:exe
echo JERICHO: linking the game exe (%CONF%)
"%MSBUILD%" build\REDRIVER2.vcxproj /p:Configuration=%CONF% /p:Platform=x64 /m /v:m /nologo
set "ERR=%errorlevel%"
goto :done

:done
popd
exit /b %ERR%
