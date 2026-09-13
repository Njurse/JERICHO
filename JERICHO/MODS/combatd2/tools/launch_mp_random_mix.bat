@echo off
rem Multiplayer-arena launcher: a RANDOM cross-city import and a random vehicle
rem roster for the player AND for the AI opponents.
rem
rem How the roster is mixed, and why the game just does it:
rem   * carhacks imports 1-2 vehicles from another city into spare/traffic
rem     resident slots (import = slot:city:model). Those slots are built from
rem     that city's level file, so they are real, drivable vehicles.
rem   * the AI opponents already pick their car at spawn by enumerating the
rem     resident slots this level actually LOADED and taking a salted random one
rem     (combatd2 ai/opponent.c, cd2AiSpawnOne), so they mix imported cars in
rem     automatically once the imports exist.
rem   * ambient traffic draws from slots 0..4, so imports there show up as
rem     regular city traffic.
rem   * the player either gets a random local slot (-car slotN) or, half the
rem     time, a randomly chosen FOREIGN car (player_model + import into the
rem     special slot).
rem
rem Requires the combatd2 module enabled in JERICHO\CONFIG\modlist.ini
rem (bin\Release_dev\JERICHO\CONFIG\modlist.ini).
rem
rem NOTE: when it launches, this OVERWRITES bin\Release_dev\JERICHO\CONFIG\carhacks.ini
rem (the cross-city hack is off by default; this launcher switches it on and
rem rolls its imports for the session). Run it with the argument "dry" to see
rem the roll and the config it would write WITHOUT changing or launching
rem anything.
rem
rem Deliberately never uses -car slot9: that is model 11, the slot the game
rem reserves for a content-override truck, and NO city ships data for it -
rem forcing it kills the game during load (same reason as launch_mp_chicago_semi.bat).
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

rem ---- the player's car ----------------------------------------------------
rem Local slots 1..8 and 10 map through carNumLookup to models 1,2,3,4,0,8,9,10,12.
rem Slot 9 (model 11) is skipped - no data on this install.
set /a "PS=%RANDOM% %% 9 + 1"
if %PS%==9 set "PS=10"
set "CARARG=-car slot%PS%"
set "PLAYERMODEL=-1"

rem Half the time the player drives a foreign car instead: models over 5 go to
rem the special resident slot, which is why the import names slot 7.
set /a "MODE=%RANDOM% %% 2"

if %MODE%==1 (
	set /a "K=%RANDOM% %% 4"
	if !K!==0 set "PLAYERMODEL=8"
	if !K!==1 set "PLAYERMODEL=9"
	if !K!==2 set "PLAYERMODEL=10"
	if !K!==3 set "PLAYERMODEL=12"
	set "CARARG="
)

rem ---- the opponents' / traffic's share of the roster ----------------------
rem One or two imports, each a random foreign model into a random slot among
rem 0..6 (0..4 feed traffic, 5..6 are spare capacity). Model 11 is never picked,
rem and the second import always takes a different slot from the first.
set "IMPORT="
set "PREVSLOT=-1"
set /a "NIM=%RANDOM% %% 2 + 1"

for /l %%i in (1,1,%NIM%) do (
	set /a "ISLOT=%RANDOM% %% 7"
	if !ISLOT!==!PREVSLOT! set /a "ISLOT=(%RANDOM% %% 6 + ISLOT + 1) %% 7"

	rem Entry 1 draws a civ model (index 0..4), entry 2 draws a special-body one
	rem (index 5..8 = models 8,9,10,12), so the two are always different.
	set /a "IM=%RANDOM% %% 5"
	if %%i==2 set /a "IM=%RANDOM% %% 4 + 5"

	if !IM!==5 set "IM=8"
	if !IM!==6 set "IM=9"
	if !IM!==7 set "IM=10"
	if !IM!==8 set "IM=12"

	if defined IMPORT (
		set "IMPORT=!IMPORT!, !ISLOT!:%SRC%:!IM!"
	) else (
		set "IMPORT=!ISLOT!:%SRC%:!IM!"
	)

	set "PREVSLOT=!ISLOT!"
)

rem The player's foreign car goes into the special slot, so the import list
rem carries it too. Kept as flat ifs: nested parenthesised blocks with delayed
rem expansion are a batch trap ("% was unexpected at this time").
set "PLAYERIMPORT="
if %MODE%==1 set "PLAYERIMPORT=7:%SRC%:%PLAYERMODEL%"

if defined PLAYERIMPORT if defined IMPORT set "IMPORT=!IMPORT!, %PLAYERIMPORT%"
if defined PLAYERIMPORT if not defined IMPORT set "IMPORT=%PLAYERIMPORT%"

if /i "%~1"=="dry" (
	echo == roll ==
	echo   arena      : %CITYNAME% ^(arena %ARENA%^), weather %WEATHER%, time %TIME%
	echo   importing  : %SRCNAME%
	echo   player     : %CARARG% player_model=%PLAYERMODEL%
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
	echo REDRIVER2_dev.exe -nointro -mp %ARENA% -level %CITYNAME% %CARARG% -weather %WEATHER% -time %TIME% -gamemode takeadrive
	endlocal
	exit /b 0
)

> "%INI%" echo # carhacks config - written by tools\launch_mp_random_mix.bat
>>"%INI%" echo # cross-city imports: slot:city:model, city = 0 CHICAGO 1 HAVANA 2 VEGAS 3 RIO
>>"%INI%" echo cross_city_vehicles = 1
>>"%INI%" echo source_city = -1
>>"%INI%" echo player_model = %PLAYERMODEL%
>>"%INI%" echo traffic_model = -1
>>"%INI%" echo traffic_slot = 2
>>"%INI%" echo car_list = 8,9,10
>>"%INI%" echo import = %IMPORT%

echo Random mix: arena=%CITYNAME% arena=%ARENA% weather=%WEATHER% time=%TIME%
echo   importing from : %SRCNAME%
echo   player         : %CARARG% player_model=%PLAYERMODEL%
echo   roster imports : %IMPORT%
echo   (wrote %INI%)

rem ---- optional test mode: "launch_mp_random_mix.bat test [frames]" --------
rem With -frames the game exits by itself at the budget, so nothing needs killing.
rem That matters: REDRIVER2.log only flushes at close, so a kill discarded the log
rem the run existed to produce. The seed is printed so the scenario is reproducible.
set "TESTARGS="
set "FRAMES=%~2"
if not defined FRAMES set "FRAMES=1350"
if /i not "%~1"=="test" goto :notest
rem a 30-bit seed: max 1073741823, so the engine's atoi reads it back exactly
set /a "SEED=%RANDOM% * 32768 + %RANDOM%" >nul
set "TESTARGS=-frames %FRAMES% -seed %SEED%"
echo   test mode      : frames=%FRAMES% seed=%SEED%
:notest

cd /d "%EXEDIR%"
echo   running        : REDRIVER2_dev.exe -nointro -mp %ARENA% -level %CITYNAME% %CARARG% -weather %WEATHER% -time %TIME% -gamemode takeadrive %TESTARGS%
start "" "REDRIVER2_dev.exe" -nointro -mp %ARENA% -level %CITYNAME% %CARARG% -weather %WEATHER% -time %TIME% -gamemode takeadrive %TESTARGS%
endlocal
