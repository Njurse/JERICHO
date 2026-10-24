@echo off
rem ============================================================
rem  JERICHO - build_mods.bat  (thin shim)
rem
rem  Compiles every runtime "dll" addon into a loadable DLL. The
rem  logic lives in the cross-platform driver, JERICHO\build.py.
rem ============================================================
setlocal EnableExtensions

set "HERE=%~dp0"

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

%PY% "%HERE%build.py" --runtime "%HERE%" mods %*
exit /b %errorlevel%
