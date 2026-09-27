@echo off
rem ============================================================
rem  JERICHO addon SDK - build_mods.bat  (thin shim)
rem
rem  Compiles one addon folder (mod.toml + source) into a loadable
rem  binary the game picks up at runtime - the game exe is never
rem  rebuilt and no game source is needed.
rem
rem  All the logic lives in the shared cross-platform driver, build.py.
rem
rem  Usage:  build_mods.bat <mod-folder>
rem    e.g.  build_mods.bat example
rem
rem  Produces: <mod-folder>\<folder-name>.dll
rem  Install:  copy it into the game's JERICHO\MODS\<id>\ folder and
rem            enable the addon in Options -> JERICHO.
rem ============================================================
setlocal EnableExtensions

if "%~1"=="" (
    echo usage: build_mods.bat ^<mod-folder^>   ^(a folder with mod.toml + source^)
    echo   e.g.  build_mods.bat example
    exit /b 2
)

set "PY="
where python >nul 2>nul && set "PY=python"
if not defined PY where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    echo JERICHO: Python 3 was not found on PATH - it runs the build driver build.py.
    exit /b 3
)

rem the driver sits next to this shim when the SDK is shipped standalone,
rem otherwise one level up (the shared copy in the repo's JERICHO/).
set "DRIVER=%~dp0build.py"
if not exist "%DRIVER%" set "DRIVER=%~dp0..\build.py"
if not exist "%DRIVER%" (
    echo JERICHO: build.py was not found next to build_mods.bat or one level up.
    echo          For a standalone SDK, copy JERICHO\build.py into this folder.
    exit /b 4
)

%PY% "%DRIVER%" sdk %*
exit /b %errorlevel%
