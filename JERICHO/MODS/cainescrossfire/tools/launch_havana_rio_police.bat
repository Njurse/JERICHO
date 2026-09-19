@echo off
rem ============================================================================
rem launch_havana_rio_police.bat [model] [dry|test [frames]]
rem
rem   Drives RIO's police car as the player's car in HAVANA, daytime, multiplayer,
rem   with combatd2 (carhacks) the only module enabled. This is the setup for
rem   chasing the imported-car texture problems.
rem
rem   RIO's police car is model 0 (found by cycling - see VEHICLES.md, same as
rem   HAVANA's). The import puts it in HAVANA's resident slot 0, and the player's
rem   car is chosen on the command line: -car <model> makes the engine spawn the
rem   player in whichever resident slot holds that model - slot 0, the one we
rem   imported.
rem
rem   model  the model to drive. Default 0, RIO's police car. Other RIO bodies can
rem          be tried: 1..4 are the civilian cars, 8, 10 and 12 the specials
rem          (8 is the bus).
rem   dry    print the plan only, write nothing, launch nothing
rem   test   add -frames and a seed so the run exits by itself and is replayable
rem
rem   NOT -car slotN: frontend slots are offset (carNumLookup = 1,2,3,4,0,8,9,10,
rem   11,12, so -car slot6 is model 8, the bus). -car takes the MODEL number.
rem
rem   Examples:
rem     launch_havana_rio_police.bat            : model 0 (RIO police), play
rem     launch_havana_rio_police.bat 10         : model 10, play
rem     launch_havana_rio_police.bat 12 test 90 : model 12, self-exiting test run
rem     launch_havana_rio_police.bat dry        : show the plan only
rem
rem   Batch notes, because they cost real time here: no parenthesised if blocks
rem   (a var set inside one is invisible to it, and an echo containing parentheses
rem   breaks the parse of everything after the block), and redirection in the
rem   conventional 'echo text > file' order.
rem ============================================================================
setlocal

set "EXEDIR=C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
set "CFG=%EXEDIR%\JERICHO\CONFIG\carhacks.ini"

rem ---- the Rio car goes into HAVANA resident slot 0 ---------------------------
rem HAVANA has no model 0 of its own (its residents are 1 2 3 3 4), so slot 0 is
rem where the imported Rio police car lands, and -car 0 spawns the player there.
set "ISLOT=0"
set "SRC=3"
set "LEVEL=havana"

rem ---- arguments, flat ifs only ---------------------------------------------
set "MODEL=%~1"
set "MODE=%~2"
set "FRAMES=%~3"

echo %MODEL%|findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 goto :nomodel
goto :parsed
:nomodel
set "MODE=%~1"
set "MODEL=0"
:parsed

if "%MODEL%"=="" set "MODEL=0"
if "%MODE%"=="" set "MODE=play"

set "TESTARGS="
if /i not "%MODE%"=="test" goto :havetestargs
if "%FRAMES%"=="" set "FRAMES=180"
rem a 30-bit seed, so the engine's atoi reads it back exactly
set /a "SEED=%RANDOM% * 32768 + %RANDOM%" >nul
set "TESTARGS=-frames %FRAMES% -seed %SEED%"
:havetestargs

echo.
echo   level          : %LEVEL%, daytime, -mp 1
echo   modules        : cainescrossfire [carhacks] - turned on by this launcher
echo   player car     : RIO model %MODEL%  (0 = the Rio police car)
echo   import         : slot %ISLOT% to RIO model %MODEL%
if /i "%MODE%"=="test" echo   test mode      : frames=%FRAMES% seed=%SEED%
echo   config         : %CFG%
echo   command        : REDRIVER2_dev.exe -nointro -mp 1 -level %LEVEL% -car %MODEL% -weather none -time day %TESTARGS%
echo.

if /i not "%MODE%"=="dry" goto :notdry
echo   dry: nothing written, nothing launched
endlocal
exit /b 0
:notdry

rem ---- the module that provides this must actually be on ---------------------
rem Writing carhacks.ini is not enough: only cainescrossfire reads it, and the repo's
rem modlist pins that module OFF (gameplay mods are opt-in). The bin copy of the
rem modlist is what the game reads, and the frontend rewrites it from Options ->
rem JERICHO, so switching it on here is a runtime change - see _enable_module.bat.
call "%~dp0_enable_module.bat" cainescrossfire "%EXEDIR%"
echo.

rem ---- the import ------------------------------------------------------------
echo cross_city_vehicles = 1 > "%CFG%"
echo import = %ISLOT%:%SRC%:%MODEL% >> "%CFG%"

echo   wrote          : %CFG%
type "%CFG%"
echo.

cd /d "%EXEDIR%"
start "" "REDRIVER2_dev.exe" -nointro -mp 1 -level %LEVEL% -car %MODEL% -weather none -time day %TESTARGS%
endlocal
