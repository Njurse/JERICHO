@echo off
rem ============================================================
rem  JERICHO - build_mods.bat  (thin shim)
rem
rem  Compiles every runtime "dll" addon into a loadable binary.
rem  The in-game "Compile Mods" button and existing shortcuts
rem  call this name; the logic lives in the shared cross-
rem  platform driver, JERICHO/build.py.
rem ============================================================
setlocal EnableExtensions

set "PY="
where python >nul 2>nul && set "PY=python"
if not defined PY where py >nul 2>nul && set "PY=py -3"
if not defined PY (
    echo JERICHO: Python 3 was not found on PATH - it runs the build driver build.py.
    exit /b 3
)

%PY% "%~dp0build.py" mods %*
exit /b %errorlevel%
