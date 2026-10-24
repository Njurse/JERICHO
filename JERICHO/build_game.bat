@echo off
rem ============================================================
rem  JERICHO - build_game.bat  (thin shim)
rem
rem  All build logic lives in the cross-platform driver,
rem  JERICHO\build.py. This script only finds Python and forwards
rem  the old in-game signature to it:
rem
rem    build_game.bat <src_rebuild_dir> <configuration> <step> [id]
rem    step = premake | mod <id> | exe | all
rem
rem  An empty <src_rebuild_dir> is fine: the driver walks up from
rem  its own folder and finds the tree (runtime JERICHO or repo).
rem ============================================================
setlocal EnableExtensions

set "HERE=%~dp0"
set "SRC=%~1"
set "CONF=%~2"
set "STEP=%~3"
set "MODID=%~4"

if not defined CONF set "CONF=Release"
if not defined STEP set "STEP=all"

rem ---- find Python 3 ----------------------------------------------------
set "PY="
where py >nul 2>nul
if not errorlevel 1 set "PY=py -3"
if not defined PY (
    where python >nul 2>nul
    if not errorlevel 1 set "PY=python"
)
if not defined PY (
    echo JERICHO: Python 3 was not found - install it and put it on PATH.
    exit /b 3
)

%PY% "%HERE%build.py" --runtime "%HERE%" --config "%CONF%" game %STEP% "%MODID%" --src "%SRC%"
exit /b %errorlevel%
