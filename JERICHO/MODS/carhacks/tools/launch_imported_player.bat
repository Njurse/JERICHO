@echo off
rem ============================================================================
rem launch_imported_player.bat [level] [srcCity] [model] [dry^|test [frames]]
rem
rem   ALWAYS spawns the player in an IMPORTED (cross-city) car, for chasing the
rem   imported-car VRAM work. Works for ANY level: the source city defaults to a
rem   FOREIGN one (the level's own city + 1, mod 4), so the player car genuinely
rem   comes from another city unless you ask for the level's own city.
rem
rem   Why -car and not a config flag. The engine assigns the player's model from
rem   the mission header (PlayerStartInfo[0]->model, mission.c:573) BEFORE
rem   SetupResidentModels, so a module that writes wantedCar[] from a hook arrives
rem   too late and is silently replaced by the level's own car (see
rem   carhacks/docs/HACK.md, "Traps, all of which cost time"). The player's car
rem   therefore has to be chosen on the command line: -car <model>. This writes
rem   the matching import into carhacks.ini AND passes -car <model>; InitPlayer
rem   then prefers the resident slot the import filled over a native slot with
rem   the same model number.
rem
rem   level    chicago^|havana^|lasvegas^|rio (or vegas) or 0-3. Default havana.
rem   srcCity  the FOREIGN city to import from, same spelling or 0-3.
rem            Default: the level's city + 1 (mod 4) - always foreign.
rem   model    the model number to drive, and the model the import provides.
rem            Default 0 (the police car; every city has one).
rem   dry      print the plan only, write nothing, launch nothing
rem   test     add -frames and a seed so the run exits by itself and is replayable
rem
rem   The mode word (dry/test/play) may appear anywhere; the rest fill level,
rem   srcCity and model in order - and a leading number is taken as the model.
rem
rem   Examples:
rem     launch_imported_player.bat                  : HAVANA, car from VEGAS, model 0
rem     launch_imported_player.bat rio 0 8          : RIO, car from CHICAGO, model 8
rem     launch_imported_player.bat havana 3 9 test 120
rem     launch_imported_player.bat dry               : show the plan, change nothing
rem
rem   Batch notes, because they cost real time in this tree: no parenthesised if
rem   blocks around a variable you then read, and redirection in the conventional
rem   'echo text > file' order.
rem ============================================================================
setlocal

set "EXEDIR=C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
set "CFG=%EXEDIR%\JERICHO\CONFIG\carhacks.ini"

rem the spare resident slot the import goes into (0..4 are the level's civilians,
rem 5..6 spare, 7 the special). A spare slot wins over a native one of the same
rem model number, and never clobbers a civilian.
set "ISLOT=5"

rem ---- pull the mode word (dry/test/play) out of the args, wherever it is -----
set "MODE="
set "FRAMES="
set "REST="

:scan
if "%~1"=="" goto :scandone
if /i "%~1"=="dry"  goto :m_dry
if /i "%~1"=="test" goto :m_test
if /i "%~1"=="play" goto :m_play
set "REST=%REST% %~1"
shift
goto :scan

:m_dry
set "MODE=dry"
shift
goto :scan

:m_play
set "MODE=play"
shift
goto :scan

:m_test
rem 'test' takes the frame count immediately after it, if one is there
set "MODE=test"
shift
echo %~1|findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 goto :scan
set "FRAMES=%~1"
shift
goto :scan

:scandone
if "%MODE%"=="" set "MODE=play"

rem ---- split the remaining positional args into A1 A2 A3 ----------------------
set "A1="
set "A2="
set "A3="
for /f "tokens=1,2,3" %%a in ("%REST%") do set "A1=%%a" & set "A2=%%b" & set "A3=%%c"

rem a leading number is the model (the one-argument habit); otherwise A1 is the level
echo %A1%|findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 goto :levelfirst
set "MODEL=%A1%"
set "LEVELARG=%A2%"
set "SRCARG=%A3%"
goto :argsdone

:levelfirst
set "LEVELARG=%A1%"
set "SRCARG=%A2%"
set "MODEL=%A3%"

:argsdone
if "%LEVELARG%"=="" set "LEVELARG=havana"
if "%MODEL%"=="" set "MODEL=0"

call :toindex "%LEVELARG%"
if "%IDX%"=="" goto :badlevel
set "LEVELIDX=%IDX%"
set "LEVELNAME=%NAME%"

call :toindex "%SRCARG%"
if "%IDX%"=="" goto :derivesrc
set "SRCIDX=%IDX%"
set "SRCNAME=%NAME%"
goto :srcok

:derivesrc
set /a "SRCIDX=(%LEVELIDX%+1) %% 4" >nul
call :nameof %SRCIDX%
set "SRCNAME=%NAME%"

:srcok
if /i "%MODE%"=="test" if "%FRAMES%"=="" set "FRAMES=180"

rem the level's own city is not an import - say so rather than pretend
if "%SRCIDX%"=="%LEVELIDX%" echo   note           : source city IS the level's own - this will NOT be an import

set "TESTARGS="
if /i not "%MODE%"=="test" goto :havetestargs
rem a 30-bit seed, so the engine's atoi reads it back exactly
set /a "SEED=%RANDOM% * 32768 + %RANDOM%" >nul
set "TESTARGS=-frames %FRAMES% -seed %SEED%"
:havetestargs

echo.
echo   level          : %LEVELNAME% (%LEVELIDX%), daytime
echo   modules        : carhacks - turned on by this launcher
echo   player car     : %SRCNAME% model %MODEL%  (imported into resident slot %ISLOT%)
echo   import         : import = %ISLOT%:%SRCIDX%:%MODEL%
if /i "%MODE%"=="test" echo   test mode      : frames=%FRAMES% seed=%SEED%
echo   config         : %CFG%
echo   command        : JERICHO_dev.exe -nointro -level %LEVELNAME% -car %MODEL% -weather none -time day %TESTARGS%
echo.

rem ---- does the SOURCE city even ship this model? --------------------------
rem A no-op here is the worst outcome: the import falls back, the engine logs "car
rem model N has no data in this level - player car falls back to resident slot 0", and
rem the player simply keeps their car - which reads as "the script does not replace the
rem car". Models 5, 6 and 7 are a GAP in EVERY city (measured with levmodels.py on all
rem four .LEV files), so a request for one can never be satisfied. Refuse rather than
rem launch something that cannot work, and do it BEFORE the dry exit so "dry" validates.
set "LEVFILE=%EXEDIR%\DRIVER2\LEVELS\"
call :nameof %SRCIDX%
set "LEVFILE=%LEVFILE%%NAME%.LEV"
if not exist "%~dp0levmodels.py" goto :skippedcheck
if not exist "%LEVFILE%" goto :skippedcheck
python3 "%~dp0levmodels.py" --has-model %MODEL% "%LEVFILE%" >nul 2>&1
if not errorlevel 1 goto :modelok
echo.
echo   REFUSED        : %NAME% does not ship model %MODEL%, so this cannot be an import.
echo                    (models 5, 6 and 7 exist in NO city)
echo                    what %NAME% does ship:
python3 "%~dp0levmodels.py" "%LEVFILE%" 2>nul | findstr "yes"
endlocal
exit /b 2
:skippedcheck
echo   note           : levmodels.py or %LEVFILE% missing - the model check was SKIPPED
:modelok
echo.

if /i not "%MODE%"=="dry" goto :notdry
echo   dry: nothing written, nothing launched
endlocal
exit /b 0
:notdry

rem The helper lives beside this script. If %~dp0 did not resolve (the script was
rem invoked by a relative path from another folder), fail loudly instead of
rem half-running - writing the config and launching with the modules still off.
if not exist "%~dp0_enable_module.bat" goto :nohelper

rem ---- the modules that read this must actually be on ------------------------
rem Writing carhacks.ini is not enough: only the carhacks module reads it, and the
rem repo's modlist pins gameplay modules OFF. The bin copy of the modlist is what
rem the game reads, and the frontend rewrites it from Options - JERICHO, so
rem switching them on here is a runtime change - see _enable_module.bat.
call "%~dp0_enable_module.bat" carhacks "%EXEDIR%"
echo.

rem ---- the import ------------------------------------------------------------
echo cross_city_vehicles = 1 > "%CFG%"
echo import = %ISLOT%:%SRCIDX%:%MODEL% >> "%CFG%"

echo   wrote          : %CFG%
type "%CFG%"
echo.

cd /d "%EXEDIR%"
start "" "JERICHO_dev.exe" -nointro -level %LEVELNAME% -car %MODEL% -weather none -time day %TESTARGS%
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
rem :toindex <name^|0-3>  -^> IDX (0-3) and NAME (canonical); IDX empty if unknown
rem ---------------------------------------------------------------------------
:toindex
set "IDX="
set "NAME="
set "N=%~1"
if /i "%N%"=="chicago"  set "IDX=0"
if /i "%N%"=="havana"   set "IDX=1"
if /i "%N%"=="lasvegas" set "IDX=2"
if /i "%N%"=="vegas"    set "IDX=2"
if /i "%N%"=="rio"      set "IDX=3"
echo %N%|findstr /r "^[0-3]$" >nul
if not errorlevel 1 set "IDX=%N%"
if "%IDX%"=="" goto :eof
call :nameof %IDX%
goto :eof

rem ---------------------------------------------------------------------------
rem :nameof <0-3>  -^> NAME
rem ---------------------------------------------------------------------------
:nameof
set "NAME=?"
if "%~1"=="0" set "NAME=CHICAGO"
if "%~1"=="1" set "NAME=HAVANA"
if "%~1"=="2" set "NAME=VEGAS"
if "%~1"=="3" set "NAME=RIO"
goto :eof

:badlevel
echo   error          : unknown level "%LEVELARG%" - use chicago^|havana^|lasvegas^|rio or 0-3
endlocal
exit /b 2

:nohelper
echo   error          : _enable_module.bat not found next to this script
echo                    (%~dp0) - run the launcher by its full path or from its folder
endlocal
exit /b 3
