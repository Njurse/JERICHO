@echo off
rem ============================================================
rem  JERICHO - build_game.bat  (thin shim)
rem
rem  Kept for backwards compatibility: the in-game runtime
rem  (jer_compile.c) and existing shortcuts call this name. All
rem  the build logic now lives in the shared cross-platform
rem  driver, JERICHO/build.py - this banner just forwards to it.
rem
rem  Usage:  build_game.bat <src_rebuild_dir> <config> <step> [id]
rem          step = premake | mod <id> | exe | all
rem
rem  Exits 0 on success, non-zero on failure (the driver's code).
rem ============================================================
setlocal EnableExtensions

set "SRC=%~1"
set "CONF=%~2"
set "STEP=%~3"
set "MODID=%~4"

if not defined CONF set "CONF=Release_dev"
if not defined STEP set "STEP=all"

rem ---- locate Python 3 (it runs the driver) ----
set "PY="
where python >nul 2>nul && set "PY=python"
if not defined PY where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    echo JERICHO: Python 3 was not found on PATH - it runs the build driver build.py.
    exit /b 3
)

if defined SRC (
    %PY% "%~dp0build.py" --src "%SRC%" --config "%CONF%" game %STEP% %MODID%
) else (
    %PY% "%~dp0build.py" --config "%CONF%" game %STEP% %MODID%
)
exit /b %errorlevel%
