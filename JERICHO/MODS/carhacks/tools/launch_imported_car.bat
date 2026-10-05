@echo off
rem ============================================================================
rem launch_imported_car.bat [mapCity] [carCity] [slot] [ride|survival|mp]
rem                         [dry | test [frames] | play]
rem
rem   Pick three things and go: the city you DRIVE IN, the city the CAR comes
rem   from, and which of that city's cars you want. That is the whole point - the
rem   other launchers fix an import and only vary the level, which is not enough
rem   to test a transplant.
rem
rem       launch_imported_car.bat                         : HAVANA, Newcastle slot 1, ride
rem       launch_imported_car.bat havana newcastle 1      : the same, written out
rem       launch_imported_car.bat havana miami 5          : Miami's slot 5
rem       launch_imported_car.bat vegas frisco 6 survival
rem       launch_imported_car.bat havana chicago 8 mp
rem       launch_imported_car.bat havana newcastle 4 dry : slot 4 does not exist - refuses
rem       launch_imported_car.bat havana newcastle 1 test 180
rem
rem   mapCity  chicago|havana|vegas|rio (or 0-3). Default havana. Driver 1's
rem            car-data cities are NOT map cities - there is no level to drive in.
rem   carCity  chicago|havana|vegas|rio|miami|frisco|la|newyork|newcastle
rem            (or 0-8). Default newcastle.
rem   slot     the slot as the ORIGINAL game numbered it, NOT the model the engine
rem            wants. The transplant lays each city's cars onto Driver 2's
rem            compliant models, so Newcastle's slot 1 is model 1 but its slot 5 is
rem            model 3, and Miami's slot 5 is model 4. Leave it off to list them.
rem            For a Driver 2 carCity there is no transplant, so the number IS the
rem            model.
rem
rem   mode     ride (default) | survival | mp      - mp hosts a session on 1400
rem   dry      print the plan only, write nothing, launch nothing
rem   test     add -frames and a seed so the run exits by itself and is replayable
rem
rem   Why -car and not a config flag: the engine assigns the player's model from
rem   the mission header (mission.c:573) BEFORE SetupResidentModels, so a module
rem   writing wantedCar[] from a hook arrives too late and is silently replaced.
rem   The car MUST be named on the command line.
rem
rem   NOTE, and it costs real time: run this by its FULL PATH. Invoked by a
rem   relative path, %~dp0 does not resolve, and the helper beside it (and the
rem   module enabler) are silently missed - the plan looks fine and the modules
rem   stay off. This script refuses in that case instead.
rem ============================================================================
setlocal

set "EXEDIR=C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
set "CFG=%EXEDIR%\JERICHO\CONFIG\carhacks.ini"

rem the spare resident slot the import goes into (0..4 are the level's civilians).
set "ISLOT=5"

rem ---- the helpers beside this script must actually be found ------------------
if not exist "%~dp0slotmap.py" goto :nohelper
if not exist "%~dp0_enable_module.bat" goto :nohelper

rem ---- pull the mode words out, wherever they are ----------------------------
set "MODE="
set "GAMEMODE="
set "FRAMES="
set "REST="

:scan
if "%~1"=="" goto :scandone
if /i "%~1"=="dry"      goto :m_dry
if /i "%~1"=="test"     goto :m_test
if /i "%~1"=="play"     goto :m_play
if /i "%~1"=="ride"     goto :g_ride
if /i "%~1"=="survival" goto :g_surv
if /i "%~1"=="mp"       goto :g_mp
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
set "MODE=test"
shift
echo %~1|findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 goto :scan
set "FRAMES=%~1"
shift
goto :scan

:g_ride
set "GAMEMODE=ride"
shift
goto :scan
:g_surv
set "GAMEMODE=survival"
shift
goto :scan
:g_mp
set "GAMEMODE=mp"
shift
goto :scan

:scandone
if "%MODE%"=="" set "MODE=play"
if "%GAMEMODE%"=="" set "GAMEMODE=ride"

rem ---- positional: mapCity carCity slot --------------------------------------
set "MAPARG="
set "CARARG="
set "SLOTARG="
for /f "tokens=1,2,3" %%a in ("%REST%") do set "MAPARG=%%a" & set "CARARG=%%b" & set "SLOTARG=%%c"

if "%MAPARG%"==""  set "MAPARG=havana"
if "%CARARG%"==""  set "CARARG=newcastle"

call :toindex "%MAPARG%" 3
if "%IDX%"=="" goto :badmap
if %IDX% GEQ 4 echo   error          : "%MAPARG%" is a car-DATA city, not a map - there is no level to drive in.
if %IDX% GEQ 4 goto :eof
set "MAPIDX=%IDX%"
set "MAPNAME=%NAME%"

call :toindex "%CARARG%" 8
if "%IDX%"=="" goto :badcar
set "CARIDX=%IDX%"
set "CARNAME=%NAME%"

rem ---- translate the slot, and refuse a slot that does not exist -------------
set "TRANSLATED="
if "%SLOTARG%"=="" goto :listslots
set "SLOT_TRIED=%SLOTARG%"
python3 "%~dp0slotmap.py" "%CARNAME%" "%SLOTARG%" > "%TEMP%\slt.txt" 2>&1
if errorlevel 1 goto :badslot
rem Two calls, deliberately. --model prints the number ALONE for reading here, and
rem the prose form is called again purely so the human line can be echoed. Reading
rem the model out of the sentence instead meant counting its words - which is how
rem this first got an empty model out of a seven-word line.
python3 "%~dp0slotmap.py" --model "%CARNAME%" "%SLOTARG%" > "%TEMP%\sltmodel.txt" 2>&1
for /f "tokens=1" %%L in (%TEMP%\sltmodel.txt) do set "MODEL=%%L"
for /f "tokens=*" %%L in (%TEMP%\slt.txt) do set "TRANSLATED=%%L"
rem cmd re-parses an EXPANDED value, so the arrow in "slot 1 -> model 1" was
rem read as a redirect - it left a stray file called "model" in this folder
rem and the line never printed. Drop the arrow from the value, once, and every
rem use of it is safe.
set "TRANSLATED=%TRANSLATED:->=to%#"
set "TRANSLATED=%TRANSLATED:#=%"
echo   %TRANSLATED%
goto :slotok

:listslots
echo.
echo   what %CARNAME% offers (run again with one of these):
python3 "%~dp0slotmap.py" "%CARNAME%"
echo.
endlocal
exit /b 0

:badslot
echo.
type "%TEMP%\slt.txt"
echo.
echo   REFUSED: %CARNAME% has no car in slot %SLOT_TRIED%, so this cannot be an import.
endlocal
exit /b 2

:slotok

rem ---- the plan --------------------------------------------------------------
set "TESTARGS="
if /i not "%MODE%"=="test" goto :havetestargs
if "%FRAMES%"=="" set "FRAMES=180"
set /a "SEED=%RANDOM% * 32768 + %RANDOM%" >nul
set "TESTARGS=-frames %FRAMES% -seed %SEED%"
:havetestargs

set "MODEARGS="
set "HOSTARGS="
if /i "%GAMEMODE%"=="survival" set "MODEARGS=-gamemode survival"
if /i "%GAMEMODE%"=="mp"       set "HOSTARGS=-nofmv -host 1400"

if "%CARIDX%"=="%MAPIDX%" echo   note           : car city IS the map's own - this will NOT be an import

echo.
echo   map            : %MAPNAME% (%MAPIDX%), daytime
echo   car city       : %CARNAME% (%CARIDX%)
if defined TRANSLATED echo   car slot       : %TRANSLATED%
echo   player car     : model %MODEL%, imported into resident slot %ISLOT%
echo   import         : import = %ISLOT%:%CARIDX%:%MODEL%
echo   gamemode       : %GAMEMODE%
if /i "%MODE%"=="test" echo   test mode      : frames=%FRAMES%
echo   config         : %CFG%
echo   command        : JERICHO_dev.exe -nointro -level %MAPNAME% -car %MODEL% -weather none -time day %MODEARGS% %HOSTARGS% %TESTARGS%
echo.

if /i not "%MODE%"=="dry" goto :notdry
echo   dry: nothing written, nothing launched
endlocal
exit /b 0
:notdry

rem ---- the modules that read this must actually be on ------------------------
call "%~dp0_enable_module.bat" carhacks "%EXEDIR%" >nul
call "%~dp0_enable_module.bat" mp "%EXEDIR%" >nul

echo cross_city_vehicles = 1 > "%CFG%"
echo import = %ISLOT%:%CARIDX%:%MODEL% >> "%CFG%"

cd /d "%EXEDIR%"
echo   launched       : %CARNAME% slot %SLOTARG% as model %MODEL%, in %MAPNAME%
start "" JERICHO_dev.exe -nointro -nofmv -level %MAPNAME% -car %MODEL% -weather none -time day %MODEARGS% %HOSTARGS% %TESTARGS%
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
rem :toindex <name|number> <max>  -> IDX, NAME (empty if unknown)
rem ---------------------------------------------------------------------------
:toindex
set "IDX="
set "NAME="
set "N=%~1"
set "MAX=%~2"
if /i "%N%"=="chicago"   set "IDX=0"
if /i "%N%"=="havana"    set "IDX=1"
if /i "%N%"=="lasvegas"  set "IDX=2"
if /i "%N%"=="vegas"     set "IDX=2"
if /i "%N%"=="rio"       set "IDX=3"
rem Driver 1's car-data cities. No levels of their own: they are sources only.
if /i "%N%"=="miami"     set "IDX=4"
if /i "%N%"=="frisco"    set "IDX=5"
if /i "%N%"=="la"        set "IDX=6"
if /i "%N%"=="newyork"   set "IDX=7"
if /i "%N%"=="newcastle" set "IDX=8"
if /i "%N%"=="credits"   set "IDX=8"
echo %N%|findstr /r "^[0-9][0-9]*$" >nul
if not errorlevel 1 if %N% LEQ %MAX% set "IDX=%N%"
if "%IDX%"=="" goto :eof
call :nameof %IDX%
goto :eof

:nameof
set "NAME=?"
if "%~1"=="0" set "NAME=CHICAGO"
if "%~1"=="1" set "NAME=HAVANA"
if "%~1"=="2" set "NAME=VEGAS"
if "%~1"=="3" set "NAME=RIO"
if "%~1"=="4" set "NAME=MIAMI"
if "%~1"=="5" set "NAME=FRISCO"
if "%~1"=="6" set "NAME=LA"
if "%~1"=="7" set "NAME=NEWYORK"
if "%~1"=="8" set "NAME=NEWCASTLE"
goto :eof

:badmap
echo   error          : unknown map city "%MAPARG%" - use chicago^|havana^|vegas^|rio or 0-3
endlocal
exit /b 2

:badcar
echo   error          : unknown car city "%CARARG%" - use a Driver 2 city or miami^|frisco^|la^|newyork^|newcastle
endlocal
exit /b 2

:nohelper
echo   error          : slotmap.py or _enable_module.bat not found beside this script
echo                    (%~dp0)
echo                    Run the launcher by its FULL PATH - invoked relatively, %~dp0
echo                    does not resolve and this script will not half-run.
endlocal
exit /b 3
