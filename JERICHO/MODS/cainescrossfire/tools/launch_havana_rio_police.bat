@echo off
rem ============================================================================
rem launch_havana_rio_police.bat [model] [dry|test [frames]]
rem
rem   Drives a RIO special body as the player's car in HAVANA, daytime,
rem   multiplayer, with combatd2 (carhacks) the only module enabled. This is the
rem   setup for chasing the imported-car texture problems.
rem
rem   model  Rio special body to drive. Rio's specials are 8, 9, 10 and 12 - and
rem          8 is the BUS, so 9, 10 and 12 are the candidates. Default 9.
rem   dry    print what would be written and launched, change nothing, launch nothing
rem   test   add -frames and a seed so the run exits by itself and is replayable
rem
rem   NOT -car slotN: those slot numbers are offset by the special bodies
rem   (carNumLookup = 1,2,3,4,0,8,9,10,11,12, so -car slot6 is model 8, the bus).
rem   The model number here is the model number.
rem
rem   Examples:
rem     launch_havana_rio_police.bat            : model 9, play
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

rem ---- arguments, flat ifs only ---------------------------------------------
set "MODEL=%~1"
set "MODE=%~2"
set "FRAMES=%~3"

echo %MODEL%|findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 goto :nomodel
goto :parsed
:nomodel
set "MODE=%~1"
set "MODEL=9"
:parsed

if "%MODEL%"=="" set "MODEL=9"
if "%MODE%"=="" set "MODE=play"

set "TESTARGS="
if /i not "%MODE%"=="test" goto :havetestargs
if "%FRAMES%"=="" set "FRAMES=180"
rem a 30-bit seed, so the engine's atoi reads it back exactly
set /a "SEED=%RANDOM% * 32768 + %RANDOM%" >nul
set "TESTARGS=-frames %FRAMES% -seed %SEED%"
:havetestargs

echo.
echo   level          : havana, daytime, -mp 1
echo   modules        : combatd2 [carhacks] only
echo   player car     : RIO special model %MODEL%
echo   import         : slot 7 to RIO model %MODEL%
if /i "%MODE%"=="test" echo   test mode      : frames=%FRAMES% seed=%SEED%
echo   config         : %CFG%
echo   command        : REDRIVER2_dev.exe -nointro -mp 1 -level havana -car slot2 -weather none -time day %TESTARGS%
echo.

if /i not "%MODE%"=="dry" goto :notdry
echo   dry: nothing written, nothing launched
endlocal
exit /b 0
:notdry

rem ---- the import: Rio's chosen special body into the player's special slot ----
rem slot 7 is the special slot, which is the player's. The engine takes a special
rem model from residentCarModels[SPECIAL_CAR_SLOT], which is the path that works.
echo cross_city_vehicles = 1 > "%CFG%"
echo import = 7:3:%MODEL% >> "%CFG%"
echo player_model = %MODEL% >> "%CFG%"

echo   wrote          : %CFG%
type "%CFG%"
echo.

cd /d "%EXEDIR%"
start "" "REDRIVER2_dev.exe" -nointro -mp 1 -level havana -car slot2 -weather none -time day %TESTARGS%
endlocal
