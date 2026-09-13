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

rem ---- the player's car: ALWAYS foreign -----------------------------------
rem Every city has models 8, 9, 10 and 12. Model 11 additionally exists
rem everywhere except Chicago, so it only joins the pick when the source city is
rem not Chicago.
set "PICK=8"
set /a "K=%RANDOM% %% 4"
if %K%==1 set "PICK=9"
if %K%==2 set "PICK=10"
if %K%==3 set "PICK=12"
if not %SRC%==0 if %K%==0 set "PICK=11"

set "PLAYERMODEL=%PICK%"
set "CARARG="

rem ---- the opponents' / traffic's share of the roster ----------------------
rem One or two imports, entry one from the civ models and entry two from the
rem special bodies, so the two never share a slot or a model. Kept as flat ifs:
rem nested parenthesised blocks with delayed expansion are a batch trap.
set "IMPORT=7:%SRC%:%PLAYERMODEL%"
set "PREVSLOT=7"
set /a "NIM=%RANDOM% %% 2 + 1"

for /l %%i in (1,1,%NIM%) do (
	set /a "ISLOT=%RANDOM% %% 7"
	if !ISLOT!==!PREVSLOT! set /a "ISLOT=(%RANDOM% %% 6 + ISLOT + 1) %% 7"

	set /a "IM=%RANDOM% %% 5"
	if %%i==2 set /a "IM=%RANDOM% %% 4 + 5"

	if !IM!==5 set "IM=8"
	if !IM!==6 set "IM=9"
	if !IM!==7 set "IM=10"
	if !IM!==8 set "IM=12"

	if defined ROSTER (
		set "ROSTER=!ROSTER!, !ISLOT!:%SRC%:!IM!"
	) else (
		set "ROSTER=!ISLOT!:%SRC%:!IM!"
	)

	set "PREVSLOT=!ISLOT!"
)

if defined ROSTER set "IMPORT=%IMPORT%, %ROSTER%"

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
