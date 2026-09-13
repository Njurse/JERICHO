@echo off
rem Multiplayer-arena launcher: GUARANTEED foreign player car, plus a random
rem cross-city roster for the AI opponents and traffic.
rem
rem This is launch_mp_random_mix.bat with the coin flip removed. In the mix
rem launcher the player takes a local slot half the time; here the player ALWAYS
rem spawns in a vehicle imported from another city.
rem
rem How it works:
rem   * a random arena city, weather, time and arena;
rem   * a DIFFERENT city is picked as the import source (never the arena's own);
rem   * the player's car is a random body from that source city, forced through
rem     carhacks' player_model (models over 5 go to the special resident slot,
rem     which is why the import names slot 7);
rem   * 1-2 more vehicles are imported into random slots so the AI opponents and
rem     ambient traffic mix foreign cars in too - the AI picks its car at spawn
rem     from the resident slots the level actually loaded (see AI.md), and
rem     traffic draws from slots 0..4.
rem
rem Requires the combatd2 module enabled in JERICHO\CONFIG\modlist.ini
rem (bin\Release_dev\JERICHO\CONFIG\modlist.ini).
rem
rem NOTE: it OVERWRITES bin\Release_dev\JERICHO\CONFIG\carhacks.ini when it
rem launches (cross-city is off by default; this switches it on for the
rem session). Pass the argument "dry" to see the roll and the config it would
rem write without launching or changing anything.
rem
rem HONEST CAVEAT: a cross-city vehicle carries the other city's geometry and
rem palettes, but its polygons name THAT city's texture pages, which this level
rem has not loaded - so a foreign car does not yet render correctly (usually
rem invisible). That is the outstanding texture-page import, described in
rem carhacks\CROSS_CITY.md. Use this launcher as much to reproduce that as to
rem enjoy it.
rem
rem Model 11 - the body the game reserves for a content-override truck - exists
rem in Havana, Rio and Vegas but NOT in Chicago, so it is only offered when
rem Chicago is not the source. Never pass -car slot9 on a Chicago level: no data
rem ships for it there and the game dies during load.
setlocal enabledelayedexpansion

set "EXEDIR=C:\Users\Jaret\Documents\Projects\REDRIVER2\src_rebuild\bin\Release_dev"
set "INI=%EXEDIR%\JERICHO\CONFIG\carhacks.ini"

rem ---- the arena city, and a DIFFERENT city to import from -----------------
set /a "CITY=%RANDOM% %% 4"
set /a "SRC=(%RANDOM% %% 3 + %CITY% + 1) %% 4"
if %SRC%==%CITY% set /a "SRC=(%SRC%+1) %% 4"

if %CITY%==0 set "CITYNAME=chicago"
if %CITY%==1 set "CITYNAME=havana"
if %CITY%==2 set "CITYNAME=vegas"
if %CITY%==3 set "CITYNAME=rio"

if %SRC%==0 set "SRCNAME=chicago"
if %SRC%==1 set "SRCNAME=havana"
if %SRC%==2 set "SRCNAME=vegas"
if %SRC%==3 set "SRCNAME=rio"

rem ---- weather / time / which arena ----------------------------------------
set /a "W=%RANDOM% %% 3"
set /a "T=%RANDOM% %% 4"
set /a "ARENA=%RANDOM% %% 2"

if %W%==0 set "WEATHER=none"
if %W%==1 set "WEATHER=rain"
if %W%==2 set "WEATHER=wet"

if %T%==0 set "TIME=dawn"
if %T%==1 set "TIME=day"
if %T%==2 set "TIME=dusk"
if %T%==3 set "TIME=night"

rem ---- the player's car: ALWAYS foreign, from the source city's whole roster -
rem Usable bodies: 0..4 (civilian) and 8, 9, 10, 12 (special). Model 11 exists
rem everywhere EXCEPT Chicago, so it only joins the pick for another source, and
rem models 5, 6 and 7 exist in no city at all.
set "PICK=0"
set /a "K=%RANDOM% %% 9"
if %K%==1 set "PICK=1"
if %K%==2 set "PICK=2"
if %K%==3 set "PICK=3"
if %K%==4 set "PICK=4"
if %K%==5 set "PICK=8"
if %K%==6 set "PICK=9"
if %K%==7 set "PICK=10"
if %K%==8 set "PICK=12"
if not %SRC%==0 if %K%==0 set "PICK=11"

set "PLAYERMODEL=%PICK%"
set "CARARG="

rem ---- what the whole roster pulls across -----------------------------------
rem The civilian bodies are imported slot-for-slot, 0..4. That is what makes a
rem CIVILIAN foreign car work for the player: carhacks only puts a model number
rem in wantedCar, and the engine's own pass then spawns the player in whichever
rem resident slot already holds that model - so importing the whole civ range
rem guarantees the slot it lands in is the foreign one, wherever that turns out
rem to be. Traffic reads slots 0..4 too, so the city's ambient cars become the
rem source city's as well.
set "IMPORT=0:%SRC%:0, 1:%SRC%:1, 2:%SRC%:2, 3:%SRC%:3, 4:%SRC%:4"

rem A body above 5 goes to the special resident slot.
if %PLAYERMODEL% GTR 5 set "IMPORT=%IMPORT%, 7:%SRC%:%PLAYERMODEL%"

rem Plus one special body in spare slot 5, so the AI opponents have a foreign
rem special car in their pick too (they enumerate every loaded resident slot).
set /a "EXTRA=%RANDOM% %% 4"
set "XMODEL=8"
if %EXTRA%==1 set "XMODEL=9"
if %EXTRA%==2 set "XMODEL=10"
if %EXTRA%==3 set "XMODEL=12"
set "IMPORT=%IMPORT%, 5:%SRC%:%XMODEL%"

if /i "%~1"=="dry" (
	echo == roll ==
	echo   arena      : %CITYNAME% ^(arena %ARENA%^), weather %WEATHER%, time %TIME%
	echo   importing  : %SRCNAME%
	echo   player     : FOREIGN %SRCNAME% model %PLAYERMODEL% ^(no -car argument^)
	echo   roster     : %ROSTER%
	echo.
	echo == carhacks.ini that would be written to %INI% ==
	echo cross_city_vehicles = 1
	echo source_city = -1
	echo player_model = %PLAYERMODEL%
	echo traffic_model = -1
	echo traffic_slot = 2
	echo car_list = 8,9,10
	echo import = %IMPORT%
	echo.
	echo == would launch ==
	echo REDRIVER2_dev.exe -nointro -mp %ARENA% -level %CITYNAME% -weather %WEATHER% -time %TIME% -gamemode takeadrive
	endlocal
	exit /b 0
)

> "%INI%" echo # carhacks config - written by tools\launch_mp_foreign_car.bat
>>"%INI%" echo # cross-city imports: slot:city:model, city = 0 CHICAGO 1 HAVANA 2 VEGAS 3 RIO
>>"%INI%" echo cross_city_vehicles = 1
>>"%INI%" echo source_city = -1
>>"%INI%" echo player_model = %PLAYERMODEL%
>>"%INI%" echo traffic_model = -1
>>"%INI%" echo traffic_slot = 2
>>"%INI%" echo car_list = 8,9,10
>>"%INI%" echo import = %IMPORT%

echo Foreign player car: arena=%CITYNAME% arena=%ARENA% weather=%WEATHER% time=%TIME%
echo   importing from : %SRCNAME%
echo   player         : FOREIGN %SRCNAME% model %PLAYERMODEL% (no -car)
echo   roster imports : %ROSTER%
echo   (wrote %INI%)

cd /d "%EXEDIR%"
start "" "REDRIVER2_dev.exe" -nointro -mp %ARENA% -level %CITYNAME% -weather %WEATHER% -time %TIME% -gamemode takeadrive
endlocal
