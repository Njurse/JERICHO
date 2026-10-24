@echo off
rem ============================================================
rem  JERICHO - build_restart.bat
rem
rem  The follow-up to an in-game rebuild. The game cannot relink
rem  its own image, so "Compile Mods" (and the mod store's prompt)
rem  starts this script detached and then quits. This waits for
rem  that process to disappear, builds the runtime addons and the
rem  deep modules, and starts the new exe. On failure the previous
rem  exe is restored and started instead, and CONFIG/build-failed
rem  is left for the next boot to report.
rem
rem  Usage (the game passes these):
rem    build_restart.bat <pid> <src_rebuild> <configuration> <exe> [deep ids...]
rem
rem  The game starts it with CONFIG/build.log as its output; run it
rem  by hand and it prints here. It lives beside build_game.bat.
rem ============================================================
setlocal EnableExtensions

rem %0 is shifted along with the arguments below, so pin the script's own
rem directory first: %~dp0 after the shifts is the EXE's directory, not ours.
set "HERE=%~dp0"

set "WAITPID=%~1"
set "SRC=%~2"
set "CONF=%~3"
set "EXE=%~4"
shift
shift
shift
shift

if not defined WAITPID goto usage
if not defined SRC goto usage
if not defined CONF goto usage
if not defined EXE goto usage
if not exist "%EXE%" (
	echo JERICHO: no exe at "%EXE%" - nothing to rebuild.
	exit /b 2
)

for %%i in ("%EXE%") do (
	set "EXEDIR=%%~dpi"
	set "EXEOLD=%%~dpni.old"
)
set "ROOT=%EXEDIR%JERICHO"
set "LOG=%ROOT%\CONFIG\build.log"
set "FAILED=%ROOT%\CONFIG\build-failed"

echo JERICHO: waiting for the game (pid %WAITPID%) to exit...
rem No call :label here: shift has replaced %0, and cmd resolves a called label
rem through %0 -- after four shifts the label is looked for in the wrong file.
:wait
set "ALIVE="
for /f "tokens=2 delims=," %%p in ('tasklist /FI "PID eq %WAITPID%" /NH /FO CSV 2^>nul') do if "%%~p"=="%WAITPID%" set "ALIVE=1"
if defined ALIVE (
	ping -n 2 127.0.0.1 >nul 2>nul
	goto wait
)

echo JERICHO: building runtime addons and deep modules...
echo JERICHO: the full compiler output goes to JERICHO\CONFIG\build.log
>> "%LOG%" echo.
>> "%LOG%" echo [build] ---- in-game rebuild ----
rem Back up the exe rather than moving it: the game is gone, so the linker can
rem overwrite it in place, and an interrupted console before the link still
rem leaves a runnable game. On a failed link the copy is put back.
if exist "%EXEOLD%" del /q "%EXEOLD%" >nul 2>nul
copy /y "%EXE%" "%EXEOLD%" >nul 2>nul
if errorlevel 1 (
	echo JERICHO: could not back up the exe - not building.
	exit /b 3
)
if exist "%FAILED%" del /q "%FAILED%" >nul 2>nul

echo JERICHO: runtime addons...
call "%HERE%build_mods.bat" >> "%LOG%" 2>&1
if errorlevel 1 goto failed

echo JERICHO: generating the build...
call "%HERE%build_game.bat" "%SRC%" "%CONF%" premake >> "%LOG%" 2>&1
if errorlevel 1 goto failed

:modloop
if "%~1"=="" goto link
echo JERICHO: compiling %~1...
call "%HERE%build_game.bat" "%SRC%" "%CONF%" mod %~1 >> "%LOG%" 2>&1
if errorlevel 1 goto failed
shift
goto modloop

:link
echo JERICHO: linking the game...
call "%HERE%build_game.bat" "%SRC%" "%CONF%" exe >> "%LOG%" 2>&1
if errorlevel 1 goto failed

del /q "%EXEOLD%" >nul 2>nul
echo JERICHO: build OK - starting the new exe.
set "JERICHO_TEST_MODSTORE="
set "JERICHO_TEST_MODSTORE_URL="
cd /d "%EXEDIR%"
start "" "%EXE%"
exit /b 0

:failed
echo JERICHO: build FAILED - restoring the previous exe.
del /q "%EXE%" >nul 2>nul
copy /y "%EXEOLD%" "%EXE%" >nul 2>nul && del /q "%EXEOLD%" >nul 2>nul
> "%FAILED%" echo Rebuild failed. See JERICHO\CONFIG\build.log.
set "JERICHO_TEST_MODSTORE="
set "JERICHO_TEST_MODSTORE_URL="
cd /d "%EXEDIR%"
start "" "%EXE%"
exit /b 1

:usage
echo usage: build_restart.bat ^<pid^> ^<src_rebuild^> ^<configuration^> ^<exe^> [deep-mod ids...]
exit /b 2
