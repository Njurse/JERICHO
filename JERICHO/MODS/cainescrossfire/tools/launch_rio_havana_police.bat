@echo off
rem ============================================================================
rem launch_rio_havana_police.bat [model] [dry|test [frames]]
rem
rem   Drives HAVANA's police car as the player's car in RIO, daytime, multiplayer,
rem   with combatd2 (carhacks) the only module enabled. The mirror of
rem   launch_havana_rio_police.bat.
rem
rem   HAVANA's police car is model 0 (found by cycling - see VEHICLES.md). RIO
rem   already has its OWN model 0 in resident slot 3 (its residents are 1 2 3 0 4),
rem   so the import has to REPLACE that slot - the engine spawns the player in the
rem   FIRST resident slot holding the wanted model, and slot 3 would win over any
rem   spare slot. Hence slot 3 here, not a spare.
rem
rem   model  the model to drive. Default 0, HAVANA's police car. Other HAVANA
rem          bodies can be tried: 1..4 civilian, 8, 10, 11 and 12 the specials.
rem   dry    print the plan only, write nothing, launch nothing
rem   test   add -frames and a seed so the run exits by itself and is replayable
rem
rem   NOT -car slotN: frontend slots are offset (carNumLookup = 1,2,3,4,0,8,9,10,
rem   11,12, so -car slot6 is model 8, the bus). -car takes the MODEL number.
rem
rem   Examples:
rem     launch_rio_havana_police.bat            : model 0 (HAVANA police), play
rem     launch_rio_havana_police.bat 12         : model 12, play
rem     launch_rio_havana_police.bat 12 test 90 : model 12, self-exiting test run
rem     launch_rio_havana_police.bat dry        : show the plan only
rem
rem   Batch notes, because they cost real time here: no parenthesised if blocks
rem   (a var set inside one is invisible to it, and an echo containing parentheses
rem   breaks the parse of everything after the block), and redirection in the
rem   conventional 'echo text > file' order.
rem ============================================================================
setlocal

set "EXEDIR=C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
set "CFG=%EXEDIR%\JERICHO\CONFIG\carhacks.ini"

rem ---- the Havana car goes into RIO resident slot 3 --------------------------
rem RIO's own model 0 lives in slot 3, so that is the slot to replace: the engine
rem spawns the player in the first resident slot holding the wanted model, and the
rem host's own slot would otherwise win.
set "ISLOT=3"
set "SRC=1"
set "LEVEL=rio"

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
echo   modules        : combatd2 [carhacks] only
echo   player car     : HAVANA model %MODEL%  (0 = the Havana police car)
echo   import         : slot %ISLOT% to HAVANA model %MODEL%
if /i "%MODE%"=="test" echo   test mode      : frames=%FRAMES% seed=%SEED%
echo   config         : %CFG%
echo   command        : REDRIVER2_dev.exe -nointro -mp 1 -level %LEVEL% -car %MODEL% -weather none -time day %TESTARGS%
echo.

if /i not "%MODE%"=="dry" goto :notdry
echo   dry: nothing written, nothing launched
endlocal
exit /b 0
:notdry

rem ---- the import ------------------------------------------------------------
echo cross_city_vehicles = 1 > "%CFG%"
echo import = %ISLOT%:%SRC%:%MODEL% >> "%CFG%"

echo   wrote          : %CFG%
type "%CFG%"
echo.

cd /d "%EXEDIR%"
start "" "REDRIVER2_dev.exe" -nointro -mp 1 -level %LEVEL% -car %MODEL% -weather none -time day %TESTARGS%
endlocal
